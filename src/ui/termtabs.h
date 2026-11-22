/* termtabs.h — Multi-tab terminal container (cap 8, cwd badges, Ctrl+1..8, §6.D2). */
#ifndef BT_TERMTABS_H
#define BT_TERMTABS_H

#include <gtk/gtk.h>

#include "terminal.h"
#include "vault.h"

G_BEGIN_DECLS

#define BT_TERMTABS_MAX_DEFAULT 8u

typedef struct BtTermTabs BtTermTabs;

typedef void (*BtTermTabsSessionEndFn)(const gchar *session_id, gpointer user_data);

BtTermTabs *bt_termtabs_new(guint max_tabs, BtTerminalFullscreenFn on_fullscreen,
                            BtTerminalStatusFn on_status, BtTermTabsSessionEndFn on_session_end,
                            gpointer user_data);
void bt_termtabs_free(BtTermTabs *tabs);

GtkWidget *bt_termtabs_widget(BtTermTabs *tabs);

/* Opens a new tab at vault-relative folder (NULL or "" = vault root).
 * Every non-root folder passes vault_resolve() (§6.A4). Enforces max_tabs cap. */
gboolean bt_termtabs_open(BtTermTabs *tabs, const BtVault *vault, const gchar *rel_dir,
                          const gchar *active_note_rel, const gchar *session_id, GError **error);

/* Associates a session id with the active tab (for agent launches in current tab). */
void bt_termtabs_bind_active_session(BtTermTabs *tabs, const gchar *session_id);

gboolean bt_termtabs_write_active(BtTermTabs *tabs, const guint8 *data, gsize len);
void bt_termtabs_close_all(BtTermTabs *tabs);
void bt_termtabs_focus_active(BtTermTabs *tabs);
void bt_termtabs_select(BtTermTabs *tabs, guint index);
void bt_termtabs_next(BtTermTabs *tabs);

guint bt_termtabs_count(const BtTermTabs *tabs);
gboolean bt_termtabs_any_running(const BtTermTabs *tabs);
const gchar *bt_termtabs_active_cwd(const BtTermTabs *tabs);

G_END_DECLS

#endif
