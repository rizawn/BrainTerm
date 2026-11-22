/* filetree.h — Vault .md file tree sidebar with desktop context menu (§6.A1). */
#ifndef BT_FILETREE_H
#define BT_FILETREE_H

#include <gtk/gtk.h>

#include "vault.h"

G_BEGIN_DECLS

typedef struct BtFileTree BtFileTree;

typedef void (*BtFileTreeSelectFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtFileTreeOpenTermFn)(const gchar *rel_dir, gpointer user_data);

typedef enum {
  BT_TREE_ACT_OPEN = 0,
  BT_TREE_ACT_RENAME,
  BT_TREE_ACT_DUPLICATE,
  BT_TREE_ACT_DELETE,
  BT_TREE_ACT_NEW_NOTE,
  BT_TREE_ACT_OPEN_TERM,
  BT_TREE_ACT_COPY_PATH,
  BT_TREE_ACT_REVEAL,
  BT_TREE_ACT_RESCAN
} BtFileTreeAction;

typedef void (*BtFileTreeActionFn)(BtFileTreeAction action, const gchar *relpath,
                                   gboolean is_dir, gpointer user_data);

BtFileTree *bt_filetree_new(BtFileTreeSelectFn on_select, BtFileTreeOpenTermFn on_open_term,
                            gpointer user_data);
void bt_filetree_free(BtFileTree *tree);

void bt_filetree_set_action_cb(BtFileTree *tree, BtFileTreeActionFn on_action, gpointer user_data);

GtkWidget *bt_filetree_widget(BtFileTree *tree);

/* Rebuilds the tree model from vault_notes(vault). */
void bt_filetree_populate(BtFileTree *tree, const BtVault *vault);

/* Highlights the given relpath in the tree if visible. */
void bt_filetree_select_path(BtFileTree *tree, const gchar *relpath);

/* Returns newly allocated relpath of currently selected file (or NULL). */
gchar *bt_filetree_selected_path(const BtFileTree *tree);

G_END_DECLS

#endif
