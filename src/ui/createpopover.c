/* createpopover.c — Create-note confirmation popover UI (Phase L3-1d, §6.1, §14.1). */
#include "ui/createpopover.h"

#include <string.h>

struct BtCreatePopover {
  BtCreateConfirmFn on_confirm;
  BtCreateCancelFn on_cancel;
  gpointer user_data;

  BtCreatePopoverState state;
  BtCreatePlan *plan;
  gboolean cancel_notified;
  gint focused_idx; /* 0 = Create, 1 = Cancel */

  GtkWidget *popover;
  GtkWidget *title_lbl;
  GtkWidget *path_lbl;
  GtkWidget *hint_lbl;
  GtkWidget *warn_lbl;
  GtkWidget *btn_create;
  GtkWidget *btn_create_lbl;
  GtkWidget *spinner;
  GtkWidget *btn_cancel;
};

static BtCreatePlan *dup_plan(const BtCreatePlan *src)
{
  if (src == NULL)
    return NULL;
  BtCreatePlan *c = g_new0(BtCreatePlan, 1);
  c->result = src->result;
  c->target_relpath = g_strdup(src->target_relpath);
  c->note_name = g_strdup(src->note_name);
  c->display = g_strdup(src->display);
  c->anchor = g_strdup(src->anchor);
  c->block_id = g_strdup(src->block_id);
  c->reason = g_strdup(src->reason);
  c->missing_folders = g_ptr_array_new_with_free_func(g_free);
  if (src->missing_folders != NULL) {
    for (guint i = 0; i < src->missing_folders->len; i++)
      g_ptr_array_add(c->missing_folders,
                      g_strdup((const gchar *)g_ptr_array_index(src->missing_folders, i)));
  }
  c->has_folder_prefix = src->has_folder_prefix;
  c->needs_folder_create = src->needs_folder_create;
  c->has_other_same_basename = src->has_other_same_basename;
  c->stale_index = src->stale_index;
  return c;
}

static void detach_popover_parent(BtCreatePopover *cp)
{
  if (cp == NULL || cp->popover == NULL)
    return;
  if (gtk_widget_get_parent(cp->popover) != NULL)
    gtk_widget_unparent(cp->popover);
}

static void on_pop_closed(GtkPopover *pop, gpointer ud)
{
  (void)pop;
  BtCreatePopover *cp = ud;
  if (cp == NULL || cp->state == BT_CREATE_POPOVER_BUSY)
    return;
  gboolean was_shown = (cp->state == BT_CREATE_POPOVER_SHOWN);
  cp->state = BT_CREATE_POPOVER_CLOSED;
  detach_popover_parent(cp);
  if (was_shown && !cp->cancel_notified && cp->on_cancel != NULL) {
    cp->cancel_notified = TRUE;
    cp->on_cancel(cp->user_data);
  }
}

static void on_create_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_createpopover_activate_create((BtCreatePopover *)ud);
}

static void on_cancel_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_createpopover_activate_cancel((BtCreatePopover *)ud);
}

static gboolean on_pop_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                                   GdkModifierType state, gpointer ud)
{
  (void)ctrl;
  (void)keycode;
  return bt_createpopover_handle_key((BtCreatePopover *)ud, keyval, state);
}

BtCreatePopover *bt_createpopover_new(BtCreateConfirmFn on_confirm, BtCreateCancelFn on_cancel,
                                      gpointer user_data)
{
  BtCreatePopover *cp = g_new0(BtCreatePopover, 1);
  cp->on_confirm = on_confirm;
  cp->on_cancel = on_cancel;
  cp->user_data = user_data;
  cp->state = BT_CREATE_POPOVER_CLOSED;

  cp->popover = gtk_popover_new();
  gtk_widget_set_name(cp->popover, "bt-create-popover");
  gtk_popover_set_autohide(GTK_POPOVER(cp->popover), TRUE);
  g_object_ref_sink(cp->popover);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_size_request(box, 320, -1);

  cp->title_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(cp->title_lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(cp->title_lbl), TRUE);
  gtk_widget_add_css_class(cp->title_lbl, "heading");

  cp->path_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(cp->path_lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(cp->path_lbl), TRUE);
  gtk_label_set_selectable(GTK_LABEL(cp->path_lbl), TRUE);
  gtk_widget_add_css_class(cp->path_lbl, "monospace");

  cp->hint_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(cp->hint_lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(cp->hint_lbl), TRUE);
  gtk_widget_add_css_class(cp->hint_lbl, "dim-label");

  cp->warn_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(cp->warn_lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(cp->warn_lbl), TRUE);
  gtk_widget_add_css_class(cp->warn_lbl, "dim-label");
  gtk_widget_set_visible(cp->warn_lbl, FALSE);

  /* Buttons ordered Create -> Cancel for deterministic Tab focus order (CK14). */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 4);

  cp->btn_create = gtk_button_new();
  gtk_widget_set_name(cp->btn_create, "bt-create-confirm-btn");
  gtk_widget_add_css_class(cp->btn_create, "suggested-action");
  GtkWidget *cb_inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  cp->spinner = gtk_spinner_new();
  gtk_widget_set_visible(cp->spinner, FALSE);
  cp->btn_create_lbl = gtk_label_new("Create");
  gtk_box_append(GTK_BOX(cb_inner), cp->spinner);
  gtk_box_append(GTK_BOX(cb_inner), cp->btn_create_lbl);
  gtk_button_set_child(GTK_BUTTON(cp->btn_create), cb_inner);

  cp->btn_cancel = gtk_button_new_with_label("Cancel");
  gtk_widget_set_name(cp->btn_cancel, "bt-create-cancel-btn");

  gtk_box_append(GTK_BOX(btn_box), cp->btn_create);
  gtk_box_append(GTK_BOX(btn_box), cp->btn_cancel);

  gtk_box_append(GTK_BOX(box), cp->title_lbl);
  gtk_box_append(GTK_BOX(box), cp->path_lbl);
  gtk_box_append(GTK_BOX(box), cp->hint_lbl);
  gtk_box_append(GTK_BOX(box), cp->warn_lbl);
  gtk_box_append(GTK_BOX(box), btn_box);

  gtk_popover_set_child(GTK_POPOVER(cp->popover), box);

  g_signal_connect(cp->btn_create, "clicked", G_CALLBACK(on_create_clicked), cp);
  g_signal_connect(cp->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), cp);
  g_signal_connect(cp->popover, "closed", G_CALLBACK(on_pop_closed), cp);

  GtkEventController *key = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_pop_key_pressed), cp);
  gtk_widget_add_controller(cp->popover, key);

  return cp;
}

void bt_createpopover_free(BtCreatePopover *cp)
{
  if (cp == NULL)
    return;
  if (cp->popover != NULL) {
    g_signal_handlers_disconnect_by_data(cp->popover, cp);
    gtk_popover_popdown(GTK_POPOVER(cp->popover));
    detach_popover_parent(cp);
    g_clear_object(&cp->popover);
  }
  g_clear_pointer(&cp->plan, bt_createnote_plan_free);
  g_free(cp);
}

gboolean bt_createpopover_show(BtCreatePopover *cp, GtkWidget *parent_widget,
                               const GdkRectangle *pointing_rect, const BtCreatePlan *plan)
{
  if (cp == NULL || parent_widget == NULL || plan == NULL || plan->result != BT_CREATE_PLAN_OK)
    return FALSE;

  bt_createpopover_close(cp);
  g_clear_pointer(&cp->plan, bt_createnote_plan_free);
  cp->plan = dup_plan(plan);
  cp->cancel_notified = FALSE;

  g_autofree gchar *title = g_strdup_printf("Create \"%s\"?",
                                            plan->note_name ? plan->note_name : "");
  gtk_label_set_text(GTK_LABEL(cp->title_lbl), title);
  gtk_label_set_text(GTK_LABEL(cp->path_lbl), plan->target_relpath ? plan->target_relpath : "");
  gtk_label_set_text(GTK_LABEL(cp->hint_lbl),
                     plan->has_folder_prefix ? "Folder will be created if missing"
                                             : "Created next to this note");

  if (plan->has_other_same_basename && plan->note_name != NULL) {
    g_autofree gchar *warn = g_strdup_printf("Other notes named %s exist", plan->note_name);
    gtk_label_set_text(GTK_LABEL(cp->warn_lbl), warn);
    gtk_widget_set_visible(cp->warn_lbl, TRUE);
  } else {
    gtk_label_set_text(GTK_LABEL(cp->warn_lbl), "");
    gtk_widget_set_visible(cp->warn_lbl, FALSE);
  }

  bt_createpopover_set_busy(cp, FALSE);
  cp->state = BT_CREATE_POPOVER_SHOWN;
  cp->focused_idx = 0;

  detach_popover_parent(cp);
  gtk_widget_set_parent(cp->popover, parent_widget);
  if (pointing_rect != NULL)
    gtk_popover_set_pointing_to(GTK_POPOVER(cp->popover), pointing_rect);
  gtk_popover_popup(GTK_POPOVER(cp->popover));
  gtk_widget_grab_focus(cp->btn_create);
  return TRUE;
}

void bt_createpopover_set_busy(BtCreatePopover *cp, gboolean busy)
{
  if (cp == NULL)
    return;
  if (busy) {
    cp->state = BT_CREATE_POPOVER_BUSY;
    gtk_popover_set_autohide(GTK_POPOVER(cp->popover), FALSE);
    gtk_widget_set_sensitive(cp->btn_create, FALSE);
    gtk_widget_set_sensitive(cp->btn_cancel, FALSE);
    gtk_label_set_text(GTK_LABEL(cp->btn_create_lbl), "Creating...");
    gtk_widget_set_visible(cp->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(cp->spinner));
  } else {
    gtk_spinner_stop(GTK_SPINNER(cp->spinner));
    gtk_widget_set_visible(cp->spinner, FALSE);
    gtk_label_set_text(GTK_LABEL(cp->btn_create_lbl), "Create");
    gtk_widget_set_sensitive(cp->btn_create, TRUE);
    gtk_widget_set_sensitive(cp->btn_cancel, TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(cp->popover), TRUE);
    if (cp->state == BT_CREATE_POPOVER_BUSY)
      cp->state = BT_CREATE_POPOVER_SHOWN;
  }
}

void bt_createpopover_close(BtCreatePopover *cp)
{
  if (cp == NULL || cp->state == BT_CREATE_POPOVER_CLOSED)
    return;
  cp->cancel_notified = TRUE;
  cp->state = BT_CREATE_POPOVER_CLOSED;
  bt_createpopover_set_busy(cp, FALSE);
  cp->state = BT_CREATE_POPOVER_CLOSED;
  gtk_popover_popdown(GTK_POPOVER(cp->popover));
  detach_popover_parent(cp);
}

BtCreatePopoverState bt_createpopover_state(const BtCreatePopover *cp)
{
  return cp ? cp->state : BT_CREATE_POPOVER_CLOSED;
}

const BtCreatePlan *bt_createpopover_plan(const BtCreatePopover *cp)
{
  return cp ? cp->plan : NULL;
}

GtkPopover *bt_createpopover_widget(const BtCreatePopover *cp)
{
  return cp ? GTK_POPOVER(cp->popover) : NULL;
}

GtkWidget *bt_createpopover_create_button(const BtCreatePopover *cp)
{
  return cp ? cp->btn_create : NULL;
}

GtkWidget *bt_createpopover_cancel_button(const BtCreatePopover *cp)
{
  return cp ? cp->btn_cancel : NULL;
}

GtkWidget *bt_createpopover_focused_button(const BtCreatePopover *cp)
{
  if (cp == NULL)
    return NULL;
  if (gtk_widget_has_focus(cp->btn_cancel) || cp->focused_idx == 1)
    return cp->btn_cancel;
  return cp->btn_create;
}

const gchar *bt_createpopover_title_text(const BtCreatePopover *cp)
{
  return (cp && cp->title_lbl) ? gtk_label_get_text(GTK_LABEL(cp->title_lbl)) : NULL;
}

const gchar *bt_createpopover_path_text(const BtCreatePopover *cp)
{
  return (cp && cp->path_lbl) ? gtk_label_get_text(GTK_LABEL(cp->path_lbl)) : NULL;
}

const gchar *bt_createpopover_location_hint_text(const BtCreatePopover *cp)
{
  return (cp && cp->hint_lbl) ? gtk_label_get_text(GTK_LABEL(cp->hint_lbl)) : NULL;
}

const gchar *bt_createpopover_warning_text(const BtCreatePopover *cp)
{
  return (cp && cp->warn_lbl && gtk_widget_get_visible(cp->warn_lbl))
             ? gtk_label_get_text(GTK_LABEL(cp->warn_lbl))
             : NULL;
}

const gchar *bt_createpopover_create_button_label(const BtCreatePopover *cp)
{
  return (cp && cp->btn_create_lbl) ? gtk_label_get_text(GTK_LABEL(cp->btn_create_lbl)) : NULL;
}

void bt_createpopover_activate_create(BtCreatePopover *cp)
{
  if (cp == NULL || cp->state != BT_CREATE_POPOVER_SHOWN || cp->plan == NULL)
    return;
  bt_createpopover_set_busy(cp, TRUE);
  if (cp->on_confirm != NULL)
    cp->on_confirm(cp->plan, cp->user_data);
}

void bt_createpopover_activate_cancel(BtCreatePopover *cp)
{
  if (cp == NULL || cp->state != BT_CREATE_POPOVER_SHOWN)
    return;
  cp->cancel_notified = FALSE;
  cp->state = BT_CREATE_POPOVER_CLOSED;
  gtk_popover_popdown(GTK_POPOVER(cp->popover));
  detach_popover_parent(cp);
  if (!cp->cancel_notified && cp->on_cancel != NULL) {
    cp->cancel_notified = TRUE;
    cp->on_cancel(cp->user_data);
  }
}

gboolean bt_createpopover_handle_key(BtCreatePopover *cp, guint keyval, GdkModifierType state)
{
  (void)state;
  if (cp == NULL || cp->state == BT_CREATE_POPOVER_CLOSED)
    return FALSE;
  if (cp->state == BT_CREATE_POPOVER_BUSY) {
    if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
        keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab || keyval == GDK_KEY_space)
      return TRUE; /* Busy ignores Esc & repeat actions per §14.1 */
    return FALSE;
  }
  if (keyval == GDK_KEY_Escape) {
    bt_createpopover_activate_cancel(cp);
    return TRUE;
  }
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    if (gtk_widget_has_focus(cp->btn_cancel) || cp->focused_idx == 1)
      bt_createpopover_activate_cancel(cp);
    else
      bt_createpopover_activate_create(cp);
    return TRUE;
  }
  if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
    cp->focused_idx = (cp->focused_idx == 0 && !gtk_widget_has_focus(cp->btn_cancel)) ? 1 : 0;
    gtk_widget_grab_focus(cp->focused_idx == 1 ? cp->btn_cancel : cp->btn_create);
    return TRUE;
  }
  return FALSE;
}
