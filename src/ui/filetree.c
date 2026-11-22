/* filetree.c — GtkListView + GtkTreeListModel sidebar for vault notes (§6.A1). */
#include "filetree.h"

#include <string.h>

/* ----------------------------------------------------- BtTreeItem GObject */

#define BT_TYPE_TREE_ITEM (bt_tree_item_get_type())
G_DECLARE_FINAL_TYPE(BtTreeItem, bt_tree_item, BT, TREE_ITEM, GObject)

struct _BtTreeItem {
  GObject parent_instance;
  gchar *name;
  gchar *relpath;
  gboolean is_dir;
  GListStore *children; /* NULL for files */
};

G_DEFINE_TYPE(BtTreeItem, bt_tree_item, G_TYPE_OBJECT)

static void bt_tree_item_finalize(GObject *obj)
{
  BtTreeItem *self = BT_TREE_ITEM(obj);
  g_free(self->name);
  g_free(self->relpath);
  g_clear_object(&self->children);
  G_OBJECT_CLASS(bt_tree_item_parent_class)->finalize(obj);
}

static void bt_tree_item_class_init(BtTreeItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = bt_tree_item_finalize;
}

static void bt_tree_item_init(BtTreeItem *self)
{
  (void)self;
}

static BtTreeItem *bt_tree_item_new(const gchar *name, const gchar *relpath, gboolean is_dir)
{
  BtTreeItem *item = g_object_new(BT_TYPE_TREE_ITEM, NULL);
  item->name = g_strdup(name);
  item->relpath = g_strdup(relpath);
  item->is_dir = is_dir;
  if (is_dir)
    item->children = g_list_store_new(BT_TYPE_TREE_ITEM);
  return item;
}

/* ----------------------------------------------------------- BtFileTree */

struct BtFileTree {
  GtkWidget *scrolled;
  GtkWidget *list_view;
  GListStore *root_store;
  GtkTreeListModel *tree_model;
  GtkSingleSelection *selection;
  BtFileTreeSelectFn on_select;
  BtFileTreeOpenTermFn on_open_term;
  gpointer user_data;
  gboolean suppress_select;
};

static GListModel *create_child_model(gpointer item, gpointer user_data)
{
  (void)user_data;
  BtTreeItem *ti = BT_TREE_ITEM(item);
  if (!ti->is_dir || ti->children == NULL)
    return NULL;
  return G_LIST_MODEL(g_object_ref(ti->children));
}

static void on_row_right_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g;
  (void)x;
  (void)y;
  if (n_press != 1)
    return;
  GtkWidget *box = ud;
  BtFileTree *tree = g_object_get_data(G_OBJECT(box), "bt-tree");
  const gchar *rel = g_object_get_data(G_OBJECT(box), "bt-relpath");
  if (tree != NULL && tree->on_open_term != NULL && rel != NULL)
    tree->on_open_term(rel, tree->user_data);
}

static void on_factory_setup(GtkListItemFactory *factory, GtkListItem *list_item, gpointer ud)
{
  (void)factory;
  GtkWidget *expander = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon = gtk_image_new();
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  g_object_set_data(G_OBJECT(box), "bt-tree", ud);
  GtkGesture *rclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect(rclick, "pressed", G_CALLBACK(on_row_right_click), box);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(rclick));
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
  gtk_list_item_set_child(list_item, expander);
}

static void on_factory_bind(GtkListItemFactory *factory, GtkListItem *list_item, gpointer ud)
{
  (void)factory;
  (void)ud;
  GtkTreeExpander *expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(list_item));
  GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(list_item));
  gtk_tree_expander_set_list_row(expander, row);

  BtTreeItem *item = BT_TREE_ITEM(gtk_tree_list_row_get_item(row));
  GtkWidget *box = gtk_tree_expander_get_child(expander);
  GtkWidget *icon = gtk_widget_get_first_child(box);
  GtkWidget *label = gtk_widget_get_next_sibling(icon);

  g_object_set_data_full(G_OBJECT(box), "bt-relpath", g_strdup(item->relpath), g_free);
  gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                               item->is_dir ? "folder-symbolic" : "text-x-generic-symbolic");
  gtk_label_set_text(GTK_LABEL(label), item->name);
  g_object_unref(item);
}

static void on_selection_changed(GtkSingleSelection *sel, GParamSpec *pspec, gpointer ud)
{
  (void)pspec;
  BtFileTree *tree = ud;
  if (tree->suppress_select || tree->on_select == NULL)
    return;
  GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_single_selection_get_selected_item(sel));
  if (row == NULL)
    return;
  BtTreeItem *item = BT_TREE_ITEM(gtk_tree_list_row_get_item(row));
  if (item != NULL && !item->is_dir && item->relpath != NULL)
    tree->on_select(item->relpath, tree->user_data);
  g_clear_object(&item);
}

static void on_row_activate(GtkListView *view, guint position, gpointer ud)
{
  (void)view;
  BtFileTree *tree = ud;
  GtkTreeListRow *row = gtk_tree_list_model_get_row(tree->tree_model, position);
  if (row == NULL)
    return;
  BtTreeItem *item = BT_TREE_ITEM(gtk_tree_list_row_get_item(row));
  if (item != NULL) {
    if (item->is_dir)
      gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
    else if (tree->on_select != NULL && item->relpath != NULL)
      tree->on_select(item->relpath, tree->user_data);
    g_object_unref(item);
  }
  g_object_unref(row);
}

BtFileTree *bt_filetree_new(BtFileTreeSelectFn on_select, BtFileTreeOpenTermFn on_open_term,
                            gpointer user_data)
{
  BtFileTree *tree = g_new0(BtFileTree, 1);
  tree->on_select = on_select;
  tree->on_open_term = on_open_term;
  tree->user_data = user_data;

  tree->root_store = g_list_store_new(BT_TYPE_TREE_ITEM);
  tree->tree_model = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(tree->root_store)),
                                             FALSE, FALSE, create_child_model, NULL, NULL);
  tree->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(tree->tree_model)));
  gtk_single_selection_set_autoselect(tree->selection, FALSE);
  gtk_single_selection_set_can_unselect(tree->selection, TRUE);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), tree);
  g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), NULL);

  tree->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(tree->selection)), factory);
  gtk_widget_add_css_class(tree->list_view, "navigation-sidebar");
  g_signal_connect(tree->selection, "notify::selected-item", G_CALLBACK(on_selection_changed), tree);
  g_signal_connect(tree->list_view, "activate", G_CALLBACK(on_row_activate), tree);

  tree->scrolled = gtk_scrolled_window_new();
  gtk_widget_set_name(tree->scrolled, "bt-filetree");
  gtk_widget_set_hexpand(tree->scrolled, TRUE);
  gtk_widget_set_vexpand(tree->scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree->scrolled), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree->scrolled), tree->list_view);
  gtk_widget_set_size_request(tree->scrolled, 190, -1);

  return tree;
}

void bt_filetree_free(BtFileTree *tree)
{
  if (tree == NULL)
    return;
  g_clear_object(&tree->selection);
  g_clear_object(&tree->tree_model);
  g_clear_object(&tree->root_store);
  g_free(tree);
}

GtkWidget *bt_filetree_widget(BtFileTree *tree)
{
  return tree->scrolled;
}

static gint compare_items(gconstpointer a, gconstpointer b, gpointer ud)
{
  (void)ud;
  const BtTreeItem *ia = BT_TREE_ITEM((gpointer)a);
  const BtTreeItem *ib = BT_TREE_ITEM((gpointer)b);
  if (ia->is_dir != ib->is_dir)
    return ia->is_dir ? -1 : 1;
  return g_ascii_strcasecmp(ia->name, ib->name);
}

static void sort_store_recursive(GListStore *store)
{
  g_list_store_sort(store, compare_items, NULL);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  for (guint i = 0; i < n; i++) {
    BtTreeItem *it = g_list_model_get_item(G_LIST_MODEL(store), i);
    if (it->is_dir && it->children != NULL)
      sort_store_recursive(it->children);
    g_object_unref(it);
  }
}

void bt_filetree_populate(BtFileTree *tree, const BtVault *vault)
{
  g_return_if_fail(tree != NULL);
  tree->suppress_select = TRUE;
  g_list_store_remove_all(tree->root_store);

  if (vault == NULL) {
    tree->suppress_select = FALSE;
    return;
  }

  const GPtrArray *notes = vault_notes(vault);
  /* Map dir relpath -> GListStore* (borrowed from tree items). */
  GHashTable *dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < notes->len; i++) {
    const gchar *rel = g_ptr_array_index(notes, i);
    gchar **parts = g_strsplit(rel, "/", -1);
    GListStore *parent = tree->root_store;
    GString *prefix = g_string_new(NULL);

    for (guint p = 0; parts[p] != NULL; p++) {
      gboolean is_last = parts[p + 1] == NULL;
      if (prefix->len > 0)
        g_string_append_c(prefix, '/');
      g_string_append(prefix, parts[p]);

      if (!is_last) {
        GListStore *sub = g_hash_table_lookup(dirs, prefix->str);
        if (sub == NULL) {
          BtTreeItem *dir_item = bt_tree_item_new(parts[p], prefix->str, TRUE);
          g_list_store_append(parent, dir_item);
          sub = dir_item->children;
          g_hash_table_insert(dirs, g_strdup(prefix->str), sub);
          g_object_unref(dir_item);
        }
        parent = sub;
      } else {
        BtTreeItem *file_item = bt_tree_item_new(parts[p], rel, FALSE);
        g_list_store_append(parent, file_item);
        g_object_unref(file_item);
      }
    }
    g_string_free(prefix, TRUE);
    g_strfreev(parts);
  }

  g_hash_table_destroy(dirs);
  sort_store_recursive(tree->root_store);
  tree->suppress_select = FALSE;
}

void bt_filetree_select_path(BtFileTree *tree, const gchar *relpath)
{
  g_return_if_fail(tree != NULL);
  if (relpath == NULL)
    return;
  tree->suppress_select = TRUE;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(tree->tree_model));
  for (guint i = 0; i < n; i++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(tree->tree_model, i);
    BtTreeItem *item = BT_TREE_ITEM(gtk_tree_list_row_get_item(row));
    gboolean match = item != NULL && !item->is_dir && g_strcmp0(item->relpath, relpath) == 0;
    g_clear_object(&item);
    g_object_unref(row);
    if (match) {
      gtk_single_selection_set_selected(tree->selection, i);
      break;
    }
  }
  tree->suppress_select = FALSE;
}
