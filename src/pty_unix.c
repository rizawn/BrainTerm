/* pty_unix.c — POSIX openpty/forkpty PTY implementation (tier-2, §5.2, §5.4). */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

struct BtPty {
  int master_fd;
  pid_t child_pid;
  guint read_src_id;
  guint write_src_id;

  guint8 *write_buf;
  gsize write_head;
  gsize write_used;

  BtPtyDataFn on_data;
  BtPtyExitFn on_exit;
  BtPtyBackpressureFn on_backpressure;
  gpointer user_data;

  gboolean alive;
  gboolean closing;
};

static gboolean on_master_readable(GIOChannel *ch, GIOCondition cond, gpointer ud)
{
  (void)ch;
  BtPty *pty = ud;
  guint8 buf[32768];
  gsize total = 0;

  while (total < BT_PTY_READ_BUDGET) {
    ssize_t n = read(pty->master_fd, buf, MIN(sizeof(buf), BT_PTY_READ_BUDGET - total));
    if (n > 0) {
      total += (gsize)n;
      if (pty->on_data != NULL)
        pty->on_data(buf, (gsize)n, pty->user_data);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      /* EOF or EIO (slave closed) */
      pty->read_src_id = 0;
      pty->alive = FALSE;
      int status = 0;
      int code = 0;
      if (pty->child_pid > 0 && waitpid(pty->child_pid, &status, WNOHANG) == pty->child_pid) {
        code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        pty->child_pid = -1;
      }
      if (!pty->closing && pty->on_exit != NULL)
        pty->on_exit(code, pty->user_data);
      return G_SOURCE_REMOVE;
    }
  }
  if ((cond & G_IO_HUP) && total == 0) {
    pty->read_src_id = 0;
    pty->alive = FALSE;
    if (!pty->closing && pty->on_exit != NULL)
      pty->on_exit(0, pty->user_data);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean on_master_writable(GIOChannel *ch, GIOCondition cond, gpointer ud)
{
  (void)ch;
  (void)cond;
  BtPty *pty = ud;
  while (pty->write_used > 0) {
    gsize contiguous = MIN(pty->write_used, BT_PTY_WRITE_CAP - pty->write_head);
    ssize_t n = write(pty->master_fd, pty->write_buf + pty->write_head, contiguous);
    if (n > 0) {
      pty->write_head = (pty->write_head + (gsize)n) % BT_PTY_WRITE_CAP;
      pty->write_used -= (gsize)n;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return G_SOURCE_CONTINUE;
    } else {
      break;
    }
  }
  pty->write_src_id = 0;
  return G_SOURCE_REMOVE;
}

BtPty *bt_pty_spawn(const BtPtySpawnOpts *opts, GError **error)
{
  g_return_val_if_fail(opts != NULL && opts->cwd != NULL, NULL);
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = (unsigned short)MAX(opts->rows, 2);
  ws.ws_col = (unsigned short)MAX(opts->cols, 2);

  int master = -1;
  pid_t pid = forkpty(&master, NULL, NULL, &ws);
  if (pid < 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "forkpty failed: %s", g_strerror(errno));
    return NULL;
  }
  if (pid == 0) {
    /* Child process */
    if (chdir(opts->cwd) != 0)
      _exit(126);
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "brainterm", 1);
    for (guint i = 0; opts->envp != NULL && opts->envp[i] != NULL; i++)
      putenv(strdup(opts->envp[i]));

    if (opts->argv != NULL && opts->argv[0] != NULL) {
      execvp(opts->argv[0], (char *const *)opts->argv);
    } else {
      const char *sh = getenv("SHELL");
      if (sh == NULL || sh[0] == '\0')
        sh = "/bin/sh";
      execlp(sh, sh, (char *)NULL);
    }
    _exit(127);
  }

  int flags = fcntl(master, F_GETFL, 0);
  fcntl(master, F_SETFL, flags | O_NONBLOCK);
  fcntl(master, F_SETFD, FD_CLOEXEC);

  BtPty *pty = g_new0(BtPty, 1);
  pty->master_fd = master;
  pty->child_pid = pid;
  pty->write_buf = g_malloc(BT_PTY_WRITE_CAP);
  pty->on_data = opts->on_data;
  pty->on_exit = opts->on_exit;
  pty->on_backpressure = opts->on_backpressure;
  pty->user_data = opts->user_data;
  pty->alive = TRUE;

  GIOChannel *ch = g_io_channel_unix_new(master);
  pty->read_src_id = g_io_add_watch_full(ch, G_PRIORITY_HIGH, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                         on_master_readable, pty, NULL);
  g_io_channel_unref(ch);
  return pty;
}

gboolean bt_pty_write(BtPty *pty, const guint8 *data, gsize len)
{
  g_return_val_if_fail(pty != NULL, FALSE);
  if (len == 0)
    return TRUE;
  if (!pty->alive || pty->closing)
    return FALSE;
  if (pty->write_used + len > BT_PTY_WRITE_CAP) {
    if (pty->on_backpressure != NULL)
      pty->on_backpressure(TRUE, pty->user_data);
    return FALSE;
  }

  gsize tail = (pty->write_head + pty->write_used) % BT_PTY_WRITE_CAP;
  gsize first = MIN(len, BT_PTY_WRITE_CAP - tail);
  memcpy(pty->write_buf + tail, data, first);
  if (len > first)
    memcpy(pty->write_buf, data + first, len - first);
  pty->write_used += len;

  if (pty->write_src_id == 0) {
    GIOChannel *ch = g_io_channel_unix_new(pty->master_fd);
    pty->write_src_id =
        g_io_add_watch_full(ch, G_PRIORITY_HIGH, G_IO_OUT, on_master_writable, pty, NULL);
    g_io_channel_unref(ch);
  }
  return TRUE;
}

gboolean bt_pty_resize(BtPty *pty, gint rows, gint cols, GError **error)
{
  g_return_val_if_fail(pty != NULL, FALSE);
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = (unsigned short)MAX(rows, 2);
  ws.ws_col = (unsigned short)MAX(cols, 2);
  if (ioctl(pty->master_fd, TIOCSWINSZ, &ws) != 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "TIOCSWINSZ failed: %s", g_strerror(errno));
    return FALSE;
  }
  return TRUE;
}

void bt_pty_close(BtPty *pty)
{
  if (pty == NULL)
    return;
  pty->closing = TRUE;
  pty->alive = FALSE;
  if (pty->read_src_id != 0) {
    g_source_remove(pty->read_src_id);
    pty->read_src_id = 0;
  }
  if (pty->write_src_id != 0) {
    g_source_remove(pty->write_src_id);
    pty->write_src_id = 0;
  }
  if (pty->master_fd >= 0) {
    close(pty->master_fd);
    pty->master_fd = -1;
  }
  if (pty->child_pid > 0) {
    kill(-pty->child_pid, SIGTERM);
    gint64 deadline = g_get_monotonic_time() + (gint64)BT_PTY_CLOSE_TIMEOUT_MS * 1000;
    int status = 0;
    while (waitpid(pty->child_pid, &status, WNOHANG) == 0) {
      if (g_get_monotonic_time() >= deadline) {
        kill(-pty->child_pid, SIGKILL);
        waitpid(pty->child_pid, &status, 0);
        break;
      }
      g_usleep(10000);
    }
    pty->child_pid = -1;
  }
  g_free(pty->write_buf);
  g_free(pty);
}

gboolean bt_pty_is_alive(const BtPty *pty)
{
  return pty != NULL && pty->alive;
}

guint64 bt_pty_child_pid(const BtPty *pty)
{
  return (pty != NULL && pty->child_pid > 0) ? (guint64)pty->child_pid : 0;
}

gboolean bt_pty_pid_alive(guint64 pid)
{
  if (pid == 0)
    return FALSE;
  return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}
