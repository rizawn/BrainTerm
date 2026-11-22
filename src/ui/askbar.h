/* askbar.h — Ctrl+E Ask / Search / Command bar (§6.B4, P6). */
#ifndef BT_ASKBAR_H
#define BT_ASKBAR_H

#include <gtk/gtk.h>

#include "index.h"

G_BEGIN_DECLS

typedef void (*BtAskOpenNoteFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtAskRunActionFn)(const gchar *action_name, gpointer user_data);
typedef void (*BtAskSendAgentFn)(const gchar *cmd_line, gpointer user_data);

void bt_askbar_show(GtkWindow *parent, BtIndex *index, BtAskOpenNoteFn on_open_note,
                    BtAskRunActionFn on_run_action, BtAskSendAgentFn on_send_agent,
                    gpointer user_data);

G_END_DECLS

#endif
