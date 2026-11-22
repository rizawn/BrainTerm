/* filetree.c — GtkListView + GtkTreeListModel sidebar for vault notes (§6.A1)
 * with desktop-standard right-click context menu (Open, Rename, Duplicate,
 * Delete, New Note, Open Terminal, Copy Path, Reveal in Explorer) & F2 rename. */
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
  GtkWidget *stack;
  GtkWidget *scrolled;
  GtkWidget *list_view;
  GtkWidget *ctx_popover;
  GListStore *root_store;
  GtkTreeListModel *tree_model;
  GtkSingleSelection *selection;
  BtFileTreeSelectFn on_select;
  BtFileTreeOpenTermFn on_open_term;
  BtFileTreeActionFn on_action;
  gpointer user_data;
  gpointer action_ud;
  gboolean suppress_select;

  gchar *ctx_relpath;
  gboolean ctx_is_dir;
};

static gboolean is_special_agent_file(const gchar *name)
{
  if (name == NULL)
    return FALSE;
  return g_ascii_strcasecmp(name, "AGENTS.md") == 0 ||
         g_ascii_strcasecmp(name, "CLAUDE.md") == 0 ||
         g_ascii_strcasecmp(name, "GEMINI.md") == 0 ||
         g_ascii_strcasecmp(name, "CODEX.md") == 0;
}

static GListModel *create_child_model(gpointer item, gpointer user_data)
{
  (void)user_data;
  BtTreeItem *ti = BT_TREE_ITEM(item);
  if (!ti->is_dir || ti->children == NULL)
    return NULL;
  return G_LIST_MODEL(g_object_ref(ti->children));
}

/* --------------------------------------------------- context menu popup -- */

static void dispatch_ctx_action(BtFileTree *tree, BtFileTreeAction act,
                                const gchar *relpath, gboolean is_dir)
{
  if (tree == NULL)
    return;
  if (tree->on_action != NULL) {
    tree->on_action(act, relpath, is_dir, tree->action_ud);
    return;
  }
  /* Legacy fallback when no action callback is wired (tests). */
  if (act == BT_TREE_ACT_OPEN && !is_dir && tree->on_select != NULL && relpath != NULL)
    tree->on_select(relpath, tree->user_data);
  else if (act == BT_TREE_ACT_OPEN_TERM && tree->on_open_term != NULL && relpath != NULL)
    tree->on_open_term(relpath, tree->user_data);
}

static void on_ctx_item_clicked(GtkButton *btn, gpointer ud)
{
  BtFileTree *tree = ud;
  BtFileTreeAction act = (BtFileTreeAction)GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(btn), "bt-ctx-act"));
  gchar *rel = g_strdup(tree->ctx_relpath);
  gboolean is_dir = tree->ctx_is_dir;
  if (tree->ctx_popover != NULL)
    gtk_popover_popdown(GTK_POPOVER(tree->ctx_popover));
  dispatch_ctx_action(tree, act, rel, is_dir);
  g_free(rel);
}

static void add_ctx_button(GtkWidget *box, BtFileTree *tree, const gchar *label,
                           BtFileTreeAction act, gboolean destructive)
{
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_add_css_class(b, "bt-ctx-item");
  if (destructive)
    gtk_widget_add_css_class(b, "bt-ctx-danger");
  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(b));
  if (GTK_IS_LABEL(child))
    gtk_label_set_xalign(GTK_LABEL(child), 0.0f);
  g_object_set_data(G_OBJECT(b), "bt-ctx-act", GINT_TO_POINTER((gint)act));
  g_signal_connect(b, "clicked", G_CALLBACK(on_ctx_item_clicked), tree);
  gtk_box_append(GTK_BOX(box), b);
}

static void add_ctx_sep(GtkWidget *box)
{
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(sep, "bt-ctx-sep");
  gtk_box_append(GTK_BOX(box), sep);
}

static void on_ctx_closed(GtkPopover *pop, gpointer ud)
{
  BtFileTree *tree = ud;
  if (tree->ctx_popover == GTK_WIDGET(pop)) {
    tree->ctx_popover = NULL;
    gtk_widget_unparent(GTK_WIDGET(pop));
  }
}

static void show_context_menu(BtFileTree *tree, GtkWidget *anchor, gdouble x, gdouble y,
                              const gchar *relpath, gboolean is_dir)
{
  if (tree->ctx_popover != NULL) {
    GtkWidget *old = tree->ctx_popover;
    tree->ctx_popover = NULL;
    gtk_popover_popdown(GTK_POPOVER(old));
    gtk_widget_unparent(old);
  }

  g_free(tree->ctx_relpath);
  tree->ctx_relpath = g_strdup(relpath);
  tree->ctx_is_dir = is_dir;

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_add_css_class(pop, "bt-ctx-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
  gtk_widget_set_halign(pop, GTK_ALIGN_START);

  GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(menu_box, "bt-ctx-menu");

  if (relpath != NULL && !is_dir) {
    add_ctx_button(menu_box, tree, "Open", BT_TREE_ACT_OPEN, FALSE);
    add_ctx_button(menu_box, tree, "Rename…   F2", BT_TREE_ACT_RENAME, FALSE);
    add_ctx_button(menu_box, tree, "Duplicate", BT_TREE_ACT_DUPLICATE, FALSE);
    add_ctx_sep(menu_box);
    add_ctx_button(menu_box, tree, "New Note here…", BT_TREE_ACT_NEW_NOTE, FALSE);
    add_ctx_button(menu_box, tree, "Open terminal here", BT_TREE_ACT_OPEN_TERM, FALSE);
    add_ctx_button(menu_box, tree, "Copy path", BT_TREE_ACT_COPY_PATH, FALSE);
    add_ctx_button(menu_box, tree, "Reveal in Explorer", BT_TREE_ACT_REVEAL, FALSE);
    add_ctx_sep(menu_box);
    add_ctx_button(menu_box, tree, "Delete…", BT_TREE_ACT_DELETE, TRUE);
  } else if (relpath != NULL && is_dir) {
    add_ctx_button(menu_box, tree, "New Note in folder…", BT_TREE_ACT_NEW_NOTE, FALSE);
    add_ctx_button(menu_box, tree, "Open terminal here", BT_TREE_ACT_OPEN_TERM, FALSE);
    add_ctx_button(menu_box, tree, "Copy path", BT_TREE_ACT_COPY_PATH, FALSE);
    add_ctx_button(menu_box, tree, "Reveal in Explorer", BT_TREE_ACT_REVEAL, FALSE);
  } else {
    add_ctx_button(menu_box, tree, "New Note…", BT_TREE_ACT_NEW_NOTE, FALSE);
    add_ctx_button(menu_box, tree, "Open terminal in vault", BT_TREE_ACT_OPEN_TERM, FALSE);
    add_ctx_sep(menu_box);
    add_ctx_button(menu_box, tree, "Rescan vault", BT_TREE_ACT_RESCAN, FALSE);
  }

  gtk_popover_set_child(GTK_POPOVER(pop), menu_box);
  gtk_widget_set_parent(pop, anchor);
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  g_signal_connect(pop, "closed", G_CALLBACK(on_ctx_closed), tree);
  tree->ctx_popover = pop;
  gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_row_right_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  if (n_press != 1)
    return;
  GtkWidget *box = ud;
  BtFileTree *tree = g_object_get_data(G_OBJECT(box), "bt-tree");
  const gchar *rel = g_object_get_data(G_OBJECT(box), "bt-relpath");
  gboolean is_dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "bt-is-dir")) != 0;
  if (tree == NULL || rel == NULL)
    return;
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
  show_context_menu(tree, box, x, y, rel, is_dir);
}

static void on_bg_right_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  if (n_press != 1)
    return;
  BtFileTree *tree = ud;
  if (tree == NULL || tree->scrolled == NULL)
    return;
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
  show_context_menu(tree, tree->scrolled, x, y, NULL, TRUE);
}

static gboolean on_tree_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                                    GdkModifierType state, gpointer ud)
{
  (void)ctrl; (void)keycode; (void)state;
  BtFileTree *tree = ud;
  if (keyval == GDK_KEY_F2) {
    gchar *sel = bt_filetree_selected_path(tree);
    if (sel != NULL) {
      dispatch_ctx_action(tree, BT_TREE_ACT_RENAME, sel, FALSE);
      g_free(sel);
      return TRUE;
    }
  } else if (keyval == GDK_KEY_Delete) {
    gchar *sel = bt_filetree_selected_path(tree);
    if (sel != NULL) {
      dispatch_ctx_action(tree, BT_TREE_ACT_DELETE, sel, FALSE);
      g_free(sel);
      return TRUE;
    }
  }
  return FALSE;
}

static void on_factory_setup(GtkListItemFactory *factory, GtkListItem *list_item, gpointer ud)
{
  (void)factory;
  GtkWidget *expander = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  GtkWidget *badge = gtk_label_new("agent");
  gtk_widget_add_css_class(badge, "bt-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(badge, FALSE);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), badge);
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
  GtkWidget *badge = gtk_widget_get_next_sibling(label);

  g_object_set_data_full(G_OBJECT(box), "bt-relpath", g_strdup(item->relpath), g_free);
  g_object_set_data(G_OBJECT(box), "bt-is-dir", GINT_TO_POINTER(item->is_dir ? 1 : 0));
  gtk_widget_remove_css_class(icon, "bt-icon-folder");
  gtk_widget_remove_css_class(icon, "bt-icon-special");

  gboolean special = !item->is_dir && is_special_agent_file(item->name);
  if (item->is_dir) {
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), "bt-folder-symbolic");
    gtk_widget_add_css_class(icon, "bt-icon-folder");
  } else if (special) {
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), "bt-agent-symbolic");
    gtk_widget_add_css_class(icon, "bt-icon-special");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), "bt-note-symbolic");
  }
  gtk_label_set_text(GTK_LABEL(label), item->name);
  gtk_widget_set_visible(badge, special);
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

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_tree_key_pressed), tree);
  gtk_widget_add_controller(tree->list_view, key);

  tree->scrolled = gtk_scrolled_window_new();
  gtk_widget_set_name(tree->scrolled, "bt-filetree");
  gtk_widget_set_hexpand(tree->scrolled, TRUE);
  gtk_widget_set_vexpand(tree->scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree->scrolled), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree->scrolled), tree->list_view);
  gtk_widget_set_size_request(tree->scrolled, 190, -1);

  GtkGesture *bg_rclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bg_rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect(bg_rclick, "pressed", G_CALLBACK(on_bg_right_click), tree);
  gtk_widget_add_controller(tree->scrolled, GTK_EVENT_CONTROLLER(bg_rclick));

  GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(empty, "bt-empty");
  gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(empty, 16);
  gtk_widget_set_margin_end(empty, 16);
  GtkWidget *eico = gtk_image_new_from_icon_name("bt-folder-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(eico), 32);
  GtkWidget *etitle = gtk_label_new("No notes in folder");
  gtk_widget_add_css_class(etitle, "bt-empty-title");
  GtkWidget *esub = gtk_label_new("Create a note or open a vault");
  gtk_label_set_wrap(GTK_LABEL(esub), TRUE);
  gtk_label_set_justify(GTK_LABEL(esub), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(empty), eico);
  gtk_box_append(GTK_BOX(empty), etitle);
  gtk_box_append(GTK_BOX(empty), esub);

  tree->stack = gtk_stack_new();
  gtk_widget_set_hexpand(tree->stack, TRUE);
  gtk_widget_set_vexpand(tree->stack, TRUE);
  gtk_stack_add_named(GTK_STACK(tree->stack), tree->scrolled, "list");
  gtk_stack_add_named(GTK_STACK(tree->stack), empty, "empty");
  gtk_stack_set_visible_child_name(GTK_STACK(tree->stack), "empty");

  return tree;
}

void bt_filetree_set_action_cb(BtFileTree *tree, BtFileTreeActionFn on_action, gpointer user_data)
{
  if (tree == NULL)
    return;
  tree->on_action = on_action;
  tree->action_ud = user_data;
}

void bt_filetree_free(BtFileTree *tree)
{
  if (tree == NULL)
    return;
  if (tree->ctx_popover != NULL) {
    gtk_widget_unparent(tree->ctx_popover);
    tree->ctx_popover = NULL;
  }
  g_free(tree->ctx_relpath);
  g_clear_object(&tree->selection);
  g_clear_object(&tree->tree_model);
  g_clear_object(&tree->root_store);
  g_free(tree);
}

GtkWidget *bt_filetree_widget(BtFileTree *tree)
{
  return tree->stack;
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
    gtk_stack_set_visible_child_name(GTK_STACK(tree->stack), "empty");
    tree->suppress_select = FALSE;
    return;
  }

  const GPtrArray *notes = vault_notes(vault);
  if (notes->len == 0) {
    gtk_stack_set_visible_child_name(GTK_STACK(tree->stack), "empty");
    tree->suppress_select = FALSE;
    return;
  }
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
  gtk_stack_set_visible_child_name(GTK_STACK(tree->stack), "list");
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

gchar *bt_filetree_selected_path(const BtFileTree *tree)
{
  if (tree == NULL || tree->selection == NULL)
    return NULL;
  GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_single_selection_get_selected_item(tree->selection));
  if (row == NULL)
    return NULL;
  BtTreeItem *item = BT_TREE_ITEM(gtk_tree_list_row_get_item(row));
  gchar *out = (item != NULL && !item->is_dir && item->relpath != NULL) ? g_strdup(item->relpath) : NULL;
  g_clear_object(&item);
  return out;
}
