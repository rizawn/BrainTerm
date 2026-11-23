/* chooserpopover.c — Ambiguity chooser popover widget (L3-2b). */
#include "ui/chooserpopover.h"

struct BtChooserPopover {
  BtChooserOpenFn on_open;
  BtChooserCopyFn on_copy;
  BtChooserCancelFn on_cancel;
  gpointer user_data;

  BtChooserModel *model;
  GtkWidget *popover;
  GtkWidget *title_lbl;
  GtkWidget *scrolled;
  GtkWidget *listbox;
  GtkWidget *more_lbl;
  GtkWidget *open_btn;
  GtkWidget *copy_btn;
  GtkWidget *cancel_btn;

  GPtrArray *row_name_lbls; /* GtkLabel * owned by row widgets */
  gboolean open;
  gboolean syncing;
};

static void sync_selection_visuals(BtChooserPopover *cp)
{
  if (cp == NULL || cp->model == NULL || cp->listbox == NULL)
    return;
  guint sel = bt_chooser_model_selected(cp->model);
  guint n = bt_chooser_model_count(cp->model);

  cp->syncing = TRUE;
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(cp->listbox), (gint)sel);
  if (row != NULL)
    gtk_list_box_select_row(GTK_LIST_BOX(cp->listbox), row);

  for (guint i = 0; i < n && cp->row_name_lbls != NULL && i < cp->row_name_lbls->len; i++) {
    GtkWidget *lbl = g_ptr_array_index(cp->row_name_lbls, i);
    if (lbl == NULL)
      continue;
    PangoAttrList *attrs = pango_attr_list_new();
    if (i == sel)
      pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(lbl), attrs);
    pango_attr_list_unref(attrs);
  }
  cp->syncing = FALSE;
}

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
  (void)lb;
  BtChooserPopover *cp = ud;
  if (cp == NULL || cp->syncing || cp->model == NULL || row == NULL)
    return;
  gint idx = gtk_list_box_row_get_index(row);
  if (idx >= 0 && bt_chooser_model_select(cp->model, (guint)idx))
    sync_selection_visuals(cp);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
  (void)lb;
  BtChooserPopover *cp = ud;
  if (cp == NULL || cp->model == NULL || row == NULL)
    return;
  gint idx = gtk_list_box_row_get_index(row);
  if (idx >= 0)
    bt_chooser_model_select(cp->model, (guint)idx);
  bt_chooserpopover_activate_open(cp);
}

static void on_open_clicked(GtkButton *btn, gpointer ud)
{
  (void)btn;
  bt_chooserpopover_activate_open((BtChooserPopover *)ud);
}

static void on_copy_clicked(GtkButton *btn, gpointer ud)
{
  (void)btn;
  bt_chooserpopover_activate_copy((BtChooserPopover *)ud);
}

static void on_cancel_clicked(GtkButton *btn, gpointer ud)
{
  (void)btn;
  bt_chooserpopover_activate_cancel((BtChooserPopover *)ud);
}

static gboolean on_popover_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                       guint keycode, GdkModifierType state, gpointer ud)
{
  (void)ctrl;
  (void)keycode;
  return bt_chooserpopover_handle_key((BtChooserPopover *)ud, keyval, state);
}

static void on_popover_closed(GtkPopover *pop, gpointer ud)
{
  (void)pop;
  BtChooserPopover *cp = ud;
  if (cp != NULL)
    cp->open = FALSE;
}

static void teardown_popover_widget(BtChooserPopover *cp)
{
  if (cp == NULL || cp->popover == NULL)
    return;
  GtkWidget *pop = cp->popover;
  cp->popover = NULL;
  g_object_remove_weak_pointer(G_OBJECT(pop), (gpointer *)&cp->popover);
  g_signal_handlers_disconnect_by_data(pop, cp);
  if (cp->listbox != NULL)
    g_signal_handlers_disconnect_by_data(cp->listbox, cp);
  if (cp->open_btn != NULL)
    g_signal_handlers_disconnect_by_data(cp->open_btn, cp);
  if (cp->copy_btn != NULL)
    g_signal_handlers_disconnect_by_data(cp->copy_btn, cp);
  if (cp->cancel_btn != NULL)
    g_signal_handlers_disconnect_by_data(cp->cancel_btn, cp);
  cp->title_lbl = NULL;
  cp->scrolled = NULL;
  cp->listbox = NULL;
  cp->more_lbl = NULL;
  cp->open_btn = NULL;
  cp->copy_btn = NULL;
  cp->cancel_btn = NULL;
  if (cp->row_name_lbls != NULL)
    g_ptr_array_set_size(cp->row_name_lbls, 0);
  if (!gtk_widget_in_destruction(pop) && gtk_widget_get_parent(pop) != NULL)
    gtk_widget_unparent(pop);
}

static void ensure_popover_built(BtChooserPopover *cp)
{
  if (cp->popover != NULL)
    return;

  GtkWidget *pop = gtk_popover_new();
  gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
  cp->popover = pop;
  g_object_add_weak_pointer(G_OBJECT(pop), (gpointer *)&cp->popover);
  g_signal_connect(pop, "closed", G_CALLBACK(on_popover_closed), cp);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);

  GtkEventController *keyc = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keyc, GTK_PHASE_CAPTURE);
  g_signal_connect(keyc, "key-pressed", G_CALLBACK(on_popover_key_pressed), cp);
  gtk_widget_add_controller(pop, keyc);

  cp->title_lbl = gtk_label_new("");
  gtk_widget_add_css_class(cp->title_lbl, "heading");
  gtk_label_set_xalign(GTK_LABEL(cp->title_lbl), 0.0f);
  gtk_label_set_max_width_chars(GTK_LABEL(cp->title_lbl), 48);
  gtk_label_set_ellipsize(GTK_LABEL(cp->title_lbl), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(box), cp->title_lbl);

  cp->listbox = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(cp->listbox), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(cp->listbox), FALSE);
  g_signal_connect(cp->listbox, "row-selected", G_CALLBACK(on_row_selected), cp);
  g_signal_connect(cp->listbox, "row-activated", G_CALLBACK(on_row_activated), cp);

  cp->scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cp->scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(cp->scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(cp->scrolled), 440);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(cp->scrolled), cp->listbox);
  gtk_box_append(GTK_BOX(box), cp->scrolled);

  cp->more_lbl = gtk_label_new("");
  gtk_widget_add_css_class(cp->more_lbl, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(cp->more_lbl), 0.0f);
  gtk_widget_set_visible(cp->more_lbl, FALSE);
  gtk_box_append(GTK_BOX(box), cp->more_lbl);

  GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btns, GTK_ALIGN_END);
  cp->cancel_btn = gtk_button_new_with_label("Cancel");
  cp->copy_btn = gtk_button_new_with_label("Copy full path link");
  cp->open_btn = gtk_button_new_with_label("Open");
  gtk_widget_add_css_class(cp->open_btn, "suggested-action");

  g_signal_connect(cp->cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), cp);
  g_signal_connect(cp->copy_btn, "clicked", G_CALLBACK(on_copy_clicked), cp);
  g_signal_connect(cp->open_btn, "clicked", G_CALLBACK(on_open_clicked), cp);

  gtk_box_append(GTK_BOX(btns), cp->cancel_btn);
  gtk_box_append(GTK_BOX(btns), cp->copy_btn);
  gtk_box_append(GTK_BOX(btns), cp->open_btn);
  gtk_box_append(GTK_BOX(box), btns);

  gtk_popover_set_child(GTK_POPOVER(pop), box);
  gtk_popover_set_default_widget(GTK_POPOVER(pop), cp->open_btn);
}

static void populate_rows(BtChooserPopover *cp)
{
  if (cp->listbox == NULL || cp->model == NULL)
    return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(cp->listbox)) != NULL)
    gtk_list_box_remove(GTK_LIST_BOX(cp->listbox), child);
  g_ptr_array_set_size(cp->row_name_lbls, 0);

  g_autofree gchar *title = bt_chooser_model_title(cp->model);
  gtk_label_set_text(GTK_LABEL(cp->title_lbl), title ? title : "");

  guint n = bt_chooser_model_count(cp->model);
  for (guint i = 0; i < n; i++) {
    const BtChooserItem *it = bt_chooser_model_item(cp->model, i);
    if (it == NULL)
      continue;
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(row_box, 4);
    gtk_widget_set_margin_bottom(row_box, 4);
    gtk_widget_set_margin_start(row_box, 4);
    gtk_widget_set_margin_end(row_box, 4);

    GtkWidget *fn = gtk_label_new(it->filename ? it->filename : "");
    gtk_label_set_xalign(GTK_LABEL(fn), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(fn), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(row_box), fn);

    const gchar *folder_txt = (it->folder != NULL && it->folder[0] != '\0') ? it->folder : ".";
    GtkWidget *fd = gtk_label_new(folder_txt);
    gtk_widget_add_css_class(fd, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(fd), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(fd), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(row_box), fd);

    gtk_list_box_append(GTK_LIST_BOX(cp->listbox), row_box);
    g_ptr_array_add(cp->row_name_lbls, fn);
  }

  g_autofree gchar *more = bt_chooser_model_footer_more(cp->model);
  if (more != NULL) {
    gtk_label_set_text(GTK_LABEL(cp->more_lbl), more);
    gtk_widget_set_visible(cp->more_lbl, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cp->scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  } else {
    gtk_label_set_text(GTK_LABEL(cp->more_lbl), "");
    gtk_widget_set_visible(cp->more_lbl, FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cp->scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_NEVER);
  }

  sync_selection_visuals(cp);
}

BtChooserPopover *bt_chooserpopover_new(BtChooserOpenFn on_open, BtChooserCopyFn on_copy,
                                        BtChooserCancelFn on_cancel, gpointer user_data)
{
  BtChooserPopover *cp = g_new0(BtChooserPopover, 1);
  cp->on_open = on_open;
  cp->on_copy = on_copy;
  cp->on_cancel = on_cancel;
  cp->user_data = user_data;
  cp->row_name_lbls = g_ptr_array_new();
  return cp;
}

void bt_chooserpopover_free(BtChooserPopover *cp)
{
  if (cp == NULL)
    return;
  teardown_popover_widget(cp);
  g_clear_pointer(&cp->model, bt_chooser_model_free);
  g_clear_pointer(&cp->row_name_lbls, g_ptr_array_unref);
  g_free(cp);
}

void bt_chooserpopover_show(BtChooserPopover *cp, GtkWidget *anchor_widget,
                            const GdkRectangle *rect, const gchar *raw_target,
                            const gchar *const *candidates)
{
  if (cp == NULL || anchor_widget == NULL)
    return;
  ensure_popover_built(cp);

  g_clear_pointer(&cp->model, bt_chooser_model_free);
  cp->model = bt_chooser_model_new(raw_target, candidates);
  populate_rows(cp);

  GtkWidget *old_parent = gtk_widget_get_parent(cp->popover);
  if (old_parent != anchor_widget) {
    if (old_parent != NULL)
      gtk_widget_unparent(cp->popover);
    gtk_widget_set_parent(cp->popover, anchor_widget);
  }

  if (rect != NULL)
    gtk_popover_set_pointing_to(GTK_POPOVER(cp->popover), rect);

  cp->open = TRUE;
  if (gtk_widget_get_mapped(anchor_widget)) {
    gtk_popover_popup(GTK_POPOVER(cp->popover));
    if (cp->open_btn != NULL)
      gtk_widget_grab_focus(cp->open_btn);
  }
}

void bt_chooserpopover_close(BtChooserPopover *cp)
{
  if (cp == NULL)
    return;
  cp->open = FALSE;
  if (cp->popover != NULL)
    gtk_popover_popdown(GTK_POPOVER(cp->popover));
}

gboolean bt_chooserpopover_is_open(const BtChooserPopover *cp)
{
  return cp != NULL && cp->open;
}

guint bt_chooserpopover_selected_index(const BtChooserPopover *cp)
{
  return (cp && cp->model) ? bt_chooser_model_selected(cp->model) : 0u;
}

guint bt_chooserpopover_candidate_count(const BtChooserPopover *cp)
{
  return (cp && cp->model) ? bt_chooser_model_count(cp->model) : 0u;
}

const gchar *bt_chooserpopover_title_text(const BtChooserPopover *cp)
{
  if (cp == NULL || cp->title_lbl == NULL)
    return "";
  return gtk_label_get_text(GTK_LABEL(cp->title_lbl));
}

const gchar *bt_chooserpopover_footer_text(const BtChooserPopover *cp)
{
  if (cp == NULL || cp->more_lbl == NULL || !gtk_widget_get_visible(cp->more_lbl))
    return NULL;
  return gtk_label_get_text(GTK_LABEL(cp->more_lbl));
}

gboolean bt_chooserpopover_handle_key(BtChooserPopover *cp, guint keyval, GdkModifierType state)
{
  if (cp == NULL || !cp->open || cp->model == NULL)
    return FALSE;

  GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_ALT_MASK);
  if (mods == 0) {
    switch (keyval) {
      case GDK_KEY_Down:
      case GDK_KEY_KP_Down:
        bt_chooser_model_move_down(cp->model);
        sync_selection_visuals(cp);
        return TRUE;
      case GDK_KEY_Up:
      case GDK_KEY_KP_Up:
        bt_chooser_model_move_up(cp->model);
        sync_selection_visuals(cp);
        return TRUE;
      case GDK_KEY_Home:
      case GDK_KEY_KP_Home:
        bt_chooser_model_home(cp->model);
        sync_selection_visuals(cp);
        return TRUE;
      case GDK_KEY_End:
      case GDK_KEY_KP_End:
        bt_chooser_model_end(cp->model);
        sync_selection_visuals(cp);
        return TRUE;
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
        bt_chooserpopover_activate_open(cp);
        return TRUE;
      case GDK_KEY_Escape:
        bt_chooserpopover_activate_cancel(cp);
        return TRUE;
      default:
        return FALSE;
    }
  }
  if (mods == GDK_CONTROL_MASK && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
    bt_chooserpopover_activate_copy(cp);
    return TRUE;
  }
  return FALSE;
}

void bt_chooserpopover_activate_open(BtChooserPopover *cp)
{
  if (cp == NULL || !cp->open || cp->model == NULL)
    return;
  g_autofree gchar *rel = g_strdup(bt_chooser_model_selected_relpath(cp->model));
  BtChooserOpenFn cb = cp->on_open;
  gpointer ud = cp->user_data;
  bt_chooserpopover_close(cp);
  if (cb != NULL && rel != NULL)
    cb(rel, ud);
}

void bt_chooserpopover_activate_copy(BtChooserPopover *cp)
{
  if (cp == NULL || !cp->open || cp->model == NULL)
    return;
  guint idx = bt_chooser_model_selected(cp->model);
  g_autofree gchar *link = bt_chooser_model_copy_link(cp->model, idx);
  g_autofree gchar *status = bt_chooser_model_copy_status(cp->model, idx);
  if (cp->on_copy != NULL && link != NULL)
    cp->on_copy(link, status, cp->user_data);
}

void bt_chooserpopover_activate_cancel(BtChooserPopover *cp)
{
  if (cp == NULL || !cp->open)
    return;
  BtChooserCancelFn cb = cp->on_cancel;
  gpointer ud = cp->user_data;
  bt_chooserpopover_close(cp);
  if (cb != NULL)
    cb(ud);
}
