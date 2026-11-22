/* test_pty_cleanup.c — §6.D4 anti-zombie process sweep test + cwd + backpressure. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include "pty.h"
#include "testutil.h"

typedef struct {
  GMainLoop *loop;
  GString *captured;
  gboolean exited;
  gboolean backpressure_hit;
  gboolean timed_out;
  const gchar *stop_token;
} PtyProbe;

static void on_data(const guint8 *data, gsize len, gpointer ud)
{
  PtyProbe *p = ud;
  g_string_append_len(p->captured, (const gchar *)data, (gssize)len);
  if (p->stop_token != NULL && strstr(p->captured->str, p->stop_token) != NULL)
    g_main_loop_quit(p->loop);
}

static void on_exit(gint code, gpointer ud)
{
  (void)code;
  PtyProbe *p = ud;
  p->exited = TRUE;
  g_main_loop_quit(p->loop);
}

static void on_bp(gboolean dropped, gpointer ud)
{
  PtyProbe *p = ud;
  if (dropped)
    p->backpressure_hit = TRUE;
}

static gboolean on_timeout(gpointer ud)
{
  PtyProbe *p = ud;
  p->timed_out = TRUE;
  g_main_loop_quit(p->loop);
  return G_SOURCE_REMOVE;
}

static void pump_until(PtyProbe *p, const gchar *token, guint timeout_ms)
{
  p->stop_token = token;
  p->timed_out = FALSE;
  if (token != NULL && strstr(p->captured->str, token) != NULL)
    return;
  guint tid = g_timeout_add(timeout_ms, on_timeout, p);
  g_main_loop_run(p->loop);
  if (!p->timed_out)
    g_source_remove(tid);
  p->stop_token = NULL;
}

static void test_spawn_cwd_and_zero_orphans(void **state)
{
  (void)state;
  gchar *vault = g_dir_make_tmp("bt-pty-vault-XXXXXX", NULL);
  assert_non_null(vault);

  PtyProbe p = {
    .loop = g_main_loop_new(NULL, FALSE),
    .captured = g_string_new(NULL),
  };

#ifdef G_OS_WIN32
  const gchar *const argv[] = {"cmd.exe", "/Q", NULL};
#else
  const gchar *const argv[] = {"/bin/sh", NULL};
#endif

  BtPtySpawnOpts opts = {
    .cwd = vault,
    .argv = argv,
    .rows = 24,
    .cols = 80,
    .on_data = on_data,
    .on_exit = on_exit,
    .on_backpressure = on_bp,
    .user_data = &p,
  };

  GError *err = NULL;
  BtPty *pty = bt_pty_spawn(&opts, &err);
  if (pty == NULL)
    fail_msg("bt_pty_spawn failed: %s", err ? err->message : "?");

  guint64 shell_pid = bt_pty_child_pid(pty);
  assert_true(shell_pid != 0);
  assert_true(bt_pty_pid_alive(shell_pid));

  /* Verify cwd == vault and spawn a long-lived grandchild process inside PTY. */
#ifdef G_OS_WIN32
  const gchar *cmd =
      "cd\r\n"
      "powershell -NoProfile -Command \"$p=Start-Process -PassThru ping -ArgumentList '127.0.0.1','-n','60' -WindowStyle Hidden; Write-Output ('GRANDCHILD_PID='+$p.Id)\"\r\n";
#else
  const gchar *cmd = "pwd\nsleep 60 &\necho GRANDCHILD_PID=$!\n";
#endif
  assert_true(bt_pty_write(pty, (const guint8 *)cmd, strlen(cmd)));
  pump_until(&p, "GRANDCHILD_PID=", 10000);

  /* Give the line a brief moment to complete digits after '='. */
  pump_until(&p, "\n", 500);

  gchar *base = g_path_get_basename(vault);
  assert_non_null(strstr(p.captured->str, base));
  g_free(base);

  const gchar *marker = strstr(p.captured->str, "GRANDCHILD_PID=");
  guint64 grandchild_pid = 0;
  if (marker != NULL)
    grandchild_pid = g_ascii_strtoull(marker + strlen("GRANDCHILD_PID="), NULL, 10);

  /* Resize sanity check while running. */
  assert_true(bt_pty_resize(pty, 30, 100, NULL));

  /* Close PTY -> must kill BOTH shell and grandchild with zero orphans (§6.D4). */
  bt_pty_close(pty);

  /* Allow OS a brief beat to reap handles. */
  for (int i = 0; i < 20 && (bt_pty_pid_alive(shell_pid) ||
                             (grandchild_pid != 0 && bt_pty_pid_alive(grandchild_pid)));
       i++)
    g_usleep(50000);

  assert_false(bt_pty_pid_alive(shell_pid));
  if (grandchild_pid != 0)
    assert_false(bt_pty_pid_alive(grandchild_pid));

  g_string_free(p.captured, TRUE);
  g_main_loop_unref(p.loop);
  bt_test_rm_rf(vault);
  g_free(vault);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_spawn_cwd_and_zero_orphans),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
