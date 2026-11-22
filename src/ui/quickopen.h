/* quickopen.h — Ctrl+P fuzzy-open note dialog (§6.A2). */
#ifndef BT_QUICKOPEN_H
#define BT_QUICKOPEN_H

#include <gtk/gtk.h>

#include "vault.h"

G_BEGIN_DECLS

typedef void (*BtQuickOpenFn)(const gchar *relpath, gpointer user_data);

void bt_quickopen_show(GtkWindow *parent, const BtVault *vault, BtQuickOpenFn on_pick,
                       gpointer user_data);

G_END_DECLS

#endif
