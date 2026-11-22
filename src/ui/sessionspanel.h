/* sessionspanel.h — Sessions sidebar panel (Resume + Rollback to checkpoint, §6.D6–§6.D7). */
#ifndef BT_SESSIONSPANEL_H
#define BT_SESSIONSPANEL_H

#include <gtk/gtk.h>

#include "sessions.h"

G_BEGIN_DECLS

typedef struct BtSessionsPanel BtSessionsPanel;

typedef void (*BtSessionActionFn)(const gchar *session_id, gpointer user_data);

BtSessionsPanel *bt_sessionspanel_new(BtSessionActionFn on_resume, BtSessionActionFn on_rollback,
                                      gpointer user_data);
void bt_sessionspanel_free(BtSessionsPanel *panel);

GtkWidget *bt_sessionspanel_widget(BtSessionsPanel *panel);
void bt_sessionspanel_refresh(BtSessionsPanel *panel, const BtSessionStore *store);

G_END_DECLS

#endif
