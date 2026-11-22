/* pty.h — Cross-platform pseudo-terminal abstraction (§5.2, §5.4, §6.D1, §6.D4).
 * Implemented by pty_win.c (ConPTY + Job Object) and pty_unix.c (openpty). */
#ifndef BT_PTY_H
#define BT_PTY_H

#include <glib.h>

#include "bterror.h"

G_BEGIN_DECLS

/* Application-side non-blocking write queue cap (§5.4). */
#define BT_PTY_WRITE_CAP (1024u * 1024u)
/* Maximum bytes drained per main-loop dispatch (§5.4). */
#define BT_PTY_READ_BUDGET (256u * 1024u)
/* Anti-zombie wait timeout on close (§6.D4). */
#define BT_PTY_CLOSE_TIMEOUT_MS 2000u

typedef struct BtPty BtPty;

typedef void (*BtPtyDataFn)(const guint8 *data, gsize len, gpointer user_data);
typedef void (*BtPtyExitFn)(gint exit_code, gpointer user_data);
typedef void (*BtPtyBackpressureFn)(gboolean dropped, gpointer user_data);

typedef struct {
  const gchar *cwd;       /* required: working directory (vault root) */
  const gchar *const *argv; /* optional: NULL uses platform default shell */
  const gchar *const *envp; /* optional: extra KEY=VALUE entries */
  gint rows;
  gint cols;
  BtPtyDataFn on_data;
  BtPtyExitFn on_exit;
  BtPtyBackpressureFn on_backpressure;
  gpointer user_data;
} BtPtySpawnOpts;

/* Spawns a shell or command inside a real PTY. Callbacks always fire on the
 * GLib default main context at G_PRIORITY_HIGH (§5.4). */
BtPty *bt_pty_spawn(const BtPtySpawnOpts *opts, GError **error);

/* Non-blocking write with 1 MB backpressure cap (§5.4). Returns FALSE if the
 * write was dropped due to backpressure or closed PTY. */
gboolean bt_pty_write(BtPty *pty, const guint8 *data, gsize len);

gboolean bt_pty_resize(BtPty *pty, gint rows, gint cols, GError **error);

/* Terminates the hosted process group, waits up to 2 s, closes handles, and
 * frees all resources. Safe to call with NULL. */
void bt_pty_close(BtPty *pty);

gboolean bt_pty_is_alive(const BtPty *pty);
guint64 bt_pty_child_pid(const BtPty *pty);

/* Helper for tests: TRUE if a process with the given PID is still running. */
gboolean bt_pty_pid_alive(guint64 pid);

G_END_DECLS

#endif
