/* terminal.h — Embedded GTK4 terminal pane widget (§5.2, §5.4, §6.D1–§6.D4, Appendix A). */
#ifndef BT_TERMINAL_H
#define BT_TERMINAL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct BtTerminal BtTerminal;

typedef void (*BtTerminalFullscreenFn)(gpointer user_data);
typedef void (*BtTerminalStatusFn)(const gchar *message, gpointer user_data);
typedef void (*BtTerminalExitFn)(BtTerminal *term, gint exit_code, gpointer user_data);

BtTerminal *bt_terminal_new(BtTerminalFullscreenFn on_fullscreen, BtTerminalStatusFn on_status,
                            gpointer user_data);
void bt_terminal_set_exit_cb(BtTerminal *term, BtTerminalExitFn on_exit, gpointer user_data);
void bt_terminal_free(BtTerminal *term);

GtkWidget *bt_terminal_widget(BtTerminal *term);

/* Spawns a shell with working directory = cwd (closing any previous PTY first). */
gboolean bt_terminal_spawn(BtTerminal *term, const gchar *cwd, GError **error);
gboolean bt_terminal_spawn_with_env(BtTerminal *term, const gchar *cwd, const gchar *const *envp,
                                    GError **error);

/* Writes raw bytes into the active PTY (used by AI launcher / Resume). */
gboolean bt_terminal_write(BtTerminal *term, const guint8 *data, gsize len);

/* Cleanly terminates the PTY and hosted process group (zero orphans, §6.D4). */
void bt_terminal_close(BtTerminal *term);

void bt_terminal_focus(BtTerminal *term);
gboolean bt_terminal_is_running(const BtTerminal *term);
const gchar *bt_terminal_cwd(const BtTerminal *term);
guint64 bt_terminal_child_pid(const BtTerminal *term);

G_END_DECLS

#endif
