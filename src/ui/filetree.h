/* filetree.h — Vault .md file tree sidebar (§6.A1). */
#ifndef BT_FILETREE_H
#define BT_FILETREE_H

#include <gtk/gtk.h>

#include "vault.h"

G_BEGIN_DECLS

typedef struct BtFileTree BtFileTree;

typedef void (*BtFileTreeSelectFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtFileTreeOpenTermFn)(const gchar *rel_dir, gpointer user_data);

BtFileTree *bt_filetree_new(BtFileTreeSelectFn on_select, BtFileTreeOpenTermFn on_open_term,
                            gpointer user_data);
void bt_filetree_free(BtFileTree *tree);

GtkWidget *bt_filetree_widget(BtFileTree *tree);

/* Rebuilds the tree model from vault_notes(vault). */
void bt_filetree_populate(BtFileTree *tree, const BtVault *vault);

/* Highlights the given relpath in the tree if visible. */
void bt_filetree_select_path(BtFileTree *tree, const gchar *relpath);

G_END_DECLS

#endif
