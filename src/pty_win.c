/* pty_win.c — Windows ConPTY + Job Object PTY implementation (§5.4, §6.D4). */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "pty.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>

typedef struct {
  guint8 *buf;
  gsize cap;
  gsize head;
  gsize tail;
  gsize used;
} RingBuf;

static void ring_init(RingBuf *r, gsize cap)
{
  r->buf = g_malloc(cap);
  r->cap = cap;
  r->head = r->tail = r->used = 0;
}

static void ring_clear(RingBuf *r)
{
  g_free(r->buf);
  r->buf = NULL;
  r->cap = r->head = r->tail = r->used = 0;
}

static gsize ring_push(RingBuf *r, const guint8 *src, gsize len)
{
  gsize avail = r->cap - r->used;
  gsize n = MIN(len, avail);
  if (n == 0)
    return 0;
  gsize first = MIN(n, r->cap - r->tail);
  memcpy(r->buf + r->tail, src, first);
  if (n > first)
    memcpy(r->buf, src + first, n - first);
  r->tail = (r->tail + n) % r->cap;
  r->used += n;
  return n;
}

static gsize ring_pop(RingBuf *r, guint8 *dst, gsize max_len)
{
  gsize n = MIN(max_len, r->used);
  if (n == 0)
    return 0;
  gsize first = MIN(n, r->cap - r->head);
  memcpy(dst, r->buf + r->head, first);
  if (n > first)
    memcpy(dst + first, r->buf, n - first);
  r->head = (r->head + n) % r->cap;
  r->used -= n;
  return n;
}

struct BtPty {
  HPCON hpc;
  HANDLE h_in_write;
  HANDLE h_out_read;
  HANDLE h_process;
  HANDLE h_job;
  DWORD child_pid;

  BtPtyDataFn on_data;
  BtPtyExitFn on_exit;
  BtPtyBackpressureFn on_backpressure;
  gpointer user_data;

  /* Read pump: reader thread -> 512 KB ring -> HIGH-priority main-loop drain. */
  GThread *reader_thread;
  GMutex read_lock;
  GCond read_space_cond;
  RingBuf read_ring;
  guint8 *drain_scratch;
  gboolean read_eof;
  gint exit_code;
  gboolean dispatch_pending;
  gboolean exit_dispatched;

  /* Write pump: main thread -> 1 MB ring -> writer thread (non-blocking). */
  GThread *writer_thread;
  GMutex write_lock;
  GCond write_cond;
  RingBuf write_ring;

  gboolean closing;
  gboolean alive;
  gint refcount;
};

static void pty_unref(BtPty *pty)
{
  if (g_atomic_int_dec_and_test(&pty->refcount)) {
    ring_clear(&pty->read_ring);
    ring_clear(&pty->write_ring);
    g_free(pty->drain_scratch);
    g_mutex_clear(&pty->read_lock);
    g_cond_clear(&pty->read_space_cond);
    g_mutex_clear(&pty->write_lock);
    g_cond_clear(&pty->write_cond);
    g_free(pty);
  }
}

static void set_win_err(GError **error, const gchar *what, DWORD code)
{
  gchar *msg = g_win32_error_message((gint)code);
  g_set_error(error, BT_ERROR, BT_ERROR_IO, "%s: %s", what, msg);
  g_free(msg);
}

static gboolean main_drain_cb(gpointer ud)
{
  BtPty *pty = ud;
  gsize n = 0;
  gboolean eof = FALSE;
  gint code = 0;
  gboolean closing = FALSE;

  g_mutex_lock(&pty->read_lock);
  closing = pty->closing;
  if (!closing) {
    n = ring_pop(&pty->read_ring, pty->drain_scratch, BT_PTY_READ_BUDGET);
    if (n > 0)
      g_cond_signal(&pty->read_space_cond);
    if (pty->read_ring.used == 0 && pty->read_eof && !pty->exit_dispatched) {
      eof = TRUE;
      code = pty->exit_code;
      pty->exit_dispatched = TRUE;
      pty->alive = FALSE;
    }
    /* If more data remains in the ring, keep dispatch_pending TRUE and reschedule. */
    if (pty->read_ring.used > 0 && !pty->closing) {
      g_mutex_unlock(&pty->read_lock);
      if (n > 0 && pty->on_data != NULL)
        pty->on_data(pty->drain_scratch, n, pty->user_data);
      return G_SOURCE_CONTINUE;
    }
  }
  pty->dispatch_pending = FALSE;
  g_mutex_unlock(&pty->read_lock);

  if (!closing) {
    if (n > 0 && pty->on_data != NULL)
      pty->on_data(pty->drain_scratch, n, pty->user_data);
    if (eof && pty->on_exit != NULL)
      pty->on_exit(code, pty->user_data);
  }
  pty_unref(pty);
  return G_SOURCE_REMOVE;
}

static void schedule_drain_locked(BtPty *pty)
{
  if (!pty->dispatch_pending && !g_atomic_int_get(&pty->closing)) {
    pty->dispatch_pending = TRUE;
    g_atomic_int_inc(&pty->refcount);
    /* Always attach an async idle source at G_PRIORITY_HIGH (never use
     * g_main_context_invoke_full while holding a lock, as it invokes
     * synchronously on the caller thread if no GMainLoop is running). */
    GSource *src = g_idle_source_new();
    g_source_set_priority(src, G_PRIORITY_HIGH);
    g_source_set_callback(src, main_drain_cb, pty, NULL);
    g_source_attach(src, NULL);
    g_source_unref(src);
  }
}

static gpointer reader_thread_fn(gpointer ud)
{
  BtPty *pty = ud;
  guint8 tmp[32768];

  for (;;) {
    DWORD got = 0;
    BOOL ok = ReadFile(pty->h_out_read, tmp, (DWORD)sizeof(tmp), &got, NULL);
    if (!ok || got == 0)
      break;

    if (g_atomic_int_get(&pty->closing))
      continue; /* keep draining so ClosePseudoConsole never blocks on a full pipe */

    gsize off = 0;
    g_mutex_lock(&pty->read_lock);
    while (off < (gsize)got && !g_atomic_int_get(&pty->closing)) {
      gsize pushed = ring_push(&pty->read_ring, tmp + off, (gsize)got - off);
      if (pushed > 0) {
        off += pushed;
        schedule_drain_locked(pty);
      } else {
        g_cond_wait(&pty->read_space_cond, &pty->read_lock);
      }
    }
    g_mutex_unlock(&pty->read_lock);
  }

  DWORD code = 0;
  if (pty->h_process != NULL) {
    WaitForSingleObject(pty->h_process, 200);
    if (!GetExitCodeProcess(pty->h_process, &code))
      code = 0;
  }

  g_mutex_lock(&pty->read_lock);
  pty->read_eof = TRUE;
  pty->exit_code = (gint)code;
  schedule_drain_locked(pty);
  g_mutex_unlock(&pty->read_lock);
  return NULL;
}

static gpointer writer_thread_fn(gpointer ud)
{
  BtPty *pty = ud;
  guint8 chunk[16384];

  for (;;) {
    g_mutex_lock(&pty->write_lock);
    while (pty->write_ring.used == 0 && !pty->closing)
      g_cond_wait(&pty->write_cond, &pty->write_lock);
    if (pty->closing) {
      g_mutex_unlock(&pty->write_lock);
      break;
    }
    gsize n = ring_pop(&pty->write_ring, chunk, sizeof(chunk));
    g_mutex_unlock(&pty->write_lock);

    gsize off = 0;
    while (off < n) {
      DWORD written = 0;
      if (!WriteFile(pty->h_in_write, chunk + off, (DWORD)(n - off), &written, NULL) ||
          written == 0)
        return NULL;
      off += (gsize)written;
    }
  }
  return NULL;
}

static gchar *default_shell_cmdline(void)
{
  gchar *pwsh = g_find_program_in_path("pwsh.exe");
  if (pwsh != NULL) {
    gchar *cmd = g_strdup_printf("\"%s\" -NoLogo", pwsh);
    g_free(pwsh);
    return cmd;
  }
  gchar *ps = g_find_program_in_path("powershell.exe");
  if (ps != NULL) {
    gchar *cmd = g_strdup_printf("\"%s\" -NoLogo", ps);
    g_free(ps);
    return cmd;
  }
  return g_strdup("cmd.exe");
}

static gchar *build_cmdline(const gchar *const *argv)
{
  if (argv == NULL || argv[0] == NULL)
    return default_shell_cmdline();
  GString *s = g_string_new(NULL);
  for (guint i = 0; argv[i] != NULL; i++) {
    if (i > 0)
      g_string_append_c(s, ' ');
    gboolean need_quote = strpbrk(argv[i], " \t\"") != NULL || argv[i][0] == '\0';
    if (!need_quote) {
      g_string_append(s, argv[i]);
    } else {
      g_string_append_c(s, '"');
      for (const gchar *p = argv[i]; *p != '\0'; p++) {
        if (*p == '"')
          g_string_append(s, "\\\"");
        else
          g_string_append_c(s, *p);
      }
      g_string_append_c(s, '"');
    }
  }
  return g_string_free(s, FALSE);
}

static wchar_t *build_env_block(const gchar *const *extra)
{
  gchar **env = g_get_environ();
  env = g_environ_setenv(env, "TERM", "xterm-256color", TRUE);
  env = g_environ_setenv(env, "COLORTERM", "truecolor", TRUE);
  env = g_environ_setenv(env, "TERM_PROGRAM", "brainterm", TRUE);
  for (guint i = 0; extra != NULL && extra[i] != NULL; i++) {
    const gchar *eq = strchr(extra[i], '=');
    if (eq != NULL && eq > extra[i]) {
      gchar *k = g_strndup(extra[i], (gsize)(eq - extra[i]));
      env = g_environ_setenv(env, k, eq + 1, TRUE);
      g_free(k);
    }
  }

  GArray *wide = g_array_new(FALSE, FALSE, sizeof(wchar_t));
  for (guint i = 0; env[i] != NULL; i++) {
    glong wlen = 0;
    gunichar2 *w = g_utf8_to_utf16(env[i], -1, NULL, &wlen, NULL);
    if (w != NULL) {
      g_array_append_vals(wide, w, (guint)wlen + 1);
      g_free(w);
    }
  }
  wchar_t nul = 0;
  g_array_append_val(wide, nul);
  g_strfreev(env);
  return (wchar_t *)(gpointer)g_array_free(wide, FALSE);
}

BtPty *bt_pty_spawn(const BtPtySpawnOpts *opts, GError **error)
{
  g_return_val_if_fail(opts != NULL && opts->cwd != NULL, NULL);
  gint rows = MAX(opts->rows, 2);
  gint cols = MAX(opts->cols, 2);

  HANDLE in_read = NULL, in_write = NULL;
  HANDLE out_read = NULL, out_write = NULL;
  HPCON hpc = NULL;
  HANDLE h_job = NULL;
  STARTUPINFOEXW si;
  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  /* Explicitly clear inherited std handles so ConPTY attaches stdio even when
   * BrainTerm itself is launched with redirected pipes (e.g. CI/meson test). */
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list = NULL;
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));

  if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
    set_win_err(error, "CreatePipe failed", GetLastError());
    goto fail;
  }

  COORD size = {(SHORT)cols, (SHORT)rows};
  HRESULT hr = CreatePseudoConsole(size, in_read, out_write, 0, &hpc);
  if (FAILED(hr)) {
    set_win_err(error, "CreatePseudoConsole failed", (DWORD)hr);
    goto fail;
  }
  /* ConPTY now owns its ends of the pipes; close our duplicates. */
  CloseHandle(in_read);
  in_read = NULL;
  CloseHandle(out_write);
  out_write = NULL;

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  attr_list = g_malloc0(attr_size);
  if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) ||
      !UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc,
                                 sizeof(HPCON), NULL, NULL)) {
    set_win_err(error, "ProcThreadAttribute setup failed", GetLastError());
    goto fail;
  }
  si.lpAttributeList = attr_list;

  h_job = CreateJobObjectW(NULL, NULL);
  if (h_job != NULL) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(h_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
  }

  gchar *cmdline_utf8 = build_cmdline(opts->argv);
  wchar_t *wcmd = (wchar_t *)g_utf8_to_utf16(cmdline_utf8, -1, NULL, NULL, NULL);
  g_free(cmdline_utf8);
  wchar_t *wcwd = (wchar_t *)g_utf8_to_utf16(opts->cwd, -1, NULL, NULL, NULL);
  wchar_t *wenv = build_env_block(opts->envp);

  BOOL created = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                    CREATE_SUSPENDED,
                                wenv, wcwd, &si.StartupInfo, &pi);
  DWORD create_err = GetLastError();
  g_free(wcmd);
  g_free(wcwd);
  g_free(wenv);
  DeleteProcThreadAttributeList(attr_list);
  g_free(attr_list);
  attr_list = NULL;

  if (!created) {
    set_win_err(error, "CreateProcessW failed", create_err);
    goto fail;
  }

  if (h_job != NULL)
    AssignProcessToJobObject(h_job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  BtPty *pty = g_new0(BtPty, 1);
  pty->hpc = hpc;
  pty->h_in_write = in_write;
  pty->h_out_read = out_read;
  pty->h_process = pi.hProcess;
  pty->h_job = h_job;
  pty->child_pid = pi.dwProcessId;
  pty->on_data = opts->on_data;
  pty->on_exit = opts->on_exit;
  pty->on_backpressure = opts->on_backpressure;
  pty->user_data = opts->user_data;
  pty->alive = TRUE;
  pty->refcount = 1;

  g_mutex_init(&pty->read_lock);
  g_cond_init(&pty->read_space_cond);
  ring_init(&pty->read_ring, BT_PTY_READ_BUDGET * 2);
  pty->drain_scratch = g_malloc(BT_PTY_READ_BUDGET);

  g_mutex_init(&pty->write_lock);
  g_cond_init(&pty->write_cond);
  ring_init(&pty->write_ring, BT_PTY_WRITE_CAP);

  pty->reader_thread = g_thread_new("bt-pty-read", reader_thread_fn, pty);
  pty->writer_thread = g_thread_new("bt-pty-write", writer_thread_fn, pty);
  return pty;

fail:
  if (attr_list != NULL) {
    DeleteProcThreadAttributeList(attr_list);
    g_free(attr_list);
  }
  if (h_job != NULL)
    CloseHandle(h_job);
  if (hpc != NULL)
    ClosePseudoConsole(hpc);
  if (in_read != NULL)
    CloseHandle(in_read);
  if (in_write != NULL)
    CloseHandle(in_write);
  if (out_read != NULL)
    CloseHandle(out_read);
  if (out_write != NULL)
    CloseHandle(out_write);
  return NULL;
}

gboolean bt_pty_write(BtPty *pty, const guint8 *data, gsize len)
{
  g_return_val_if_fail(pty != NULL, FALSE);
  if (len == 0)
    return TRUE;
  g_mutex_lock(&pty->write_lock);
  if (pty->closing || !pty->alive) {
    g_mutex_unlock(&pty->write_lock);
    return FALSE;
  }
  if (pty->write_ring.used + len > pty->write_ring.cap) {
    g_mutex_unlock(&pty->write_lock);
    if (pty->on_backpressure != NULL)
      pty->on_backpressure(TRUE, pty->user_data);
    return FALSE;
  }
  ring_push(&pty->write_ring, data, len);
  g_cond_signal(&pty->write_cond);
  g_mutex_unlock(&pty->write_lock);
  return TRUE;
}

gboolean bt_pty_resize(BtPty *pty, gint rows, gint cols, GError **error)
{
  g_return_val_if_fail(pty != NULL, FALSE);
  if (pty->hpc == NULL || pty->closing)
    return FALSE;
  COORD size = {(SHORT)MAX(cols, 2), (SHORT)MAX(rows, 2)};
  HRESULT hr = ResizePseudoConsole(pty->hpc, size);
  if (FAILED(hr)) {
    set_win_err(error, "ResizePseudoConsole failed", (DWORD)hr);
    return FALSE;
  }
  return TRUE;
}

void bt_pty_close(BtPty *pty)
{
  if (pty == NULL)
    return;

  g_atomic_int_set(&pty->closing, TRUE);
  g_mutex_lock(&pty->read_lock);
  pty->alive = FALSE;
  g_cond_broadcast(&pty->read_space_cond);
  g_mutex_unlock(&pty->read_lock);

  g_mutex_lock(&pty->write_lock);
  g_cond_broadcast(&pty->write_cond);
  g_mutex_unlock(&pty->write_lock);

  /* Terminate entire hosted process tree via Job Object (§6.D4). */
  if (pty->h_job != NULL)
    TerminateJobObject(pty->h_job, 1);
  if (pty->h_process != NULL) {
    if (WaitForSingleObject(pty->h_process, BT_PTY_CLOSE_TIMEOUT_MS) == WAIT_TIMEOUT) {
      TerminateProcess(pty->h_process, 1);
      WaitForSingleObject(pty->h_process, 500);
    }
  }

  /* Close write pipe and ConPTY first: closing ConPTY terminates conhost.exe,
   * which closes its write end of h_out_read and wakes reader_thread cleanly.
   * Never CloseHandle(h_out_read) while reader_thread is in synchronous ReadFile. */
  if (pty->h_in_write != NULL) {
    CloseHandle(pty->h_in_write);
    pty->h_in_write = NULL;
  }
  if (pty->hpc != NULL) {
    ClosePseudoConsole(pty->hpc);
    pty->hpc = NULL;
  }

  if (pty->writer_thread != NULL) {
    g_thread_join(pty->writer_thread);
    pty->writer_thread = NULL;
  }
  if (pty->reader_thread != NULL) {
    g_thread_join(pty->reader_thread);
    pty->reader_thread = NULL;
  }
  if (pty->h_out_read != NULL) {
    CloseHandle(pty->h_out_read);
    pty->h_out_read = NULL;
  }

  if (pty->h_job != NULL) {
    CloseHandle(pty->h_job);
    pty->h_job = NULL;
  }
  if (pty->h_process != NULL) {
    CloseHandle(pty->h_process);
    pty->h_process = NULL;
  }

  pty_unref(pty);
}

gboolean bt_pty_is_alive(const BtPty *pty)
{
  return pty != NULL && pty->alive;
}

guint64 bt_pty_child_pid(const BtPty *pty)
{
  return pty != NULL ? (guint64)pty->child_pid : 0;
}

gboolean bt_pty_pid_alive(guint64 pid)
{
  if (pid == 0)
    return FALSE;
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (h == NULL)
    return FALSE;
  DWORD code = 0;
  gboolean alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
}
