/* sessionspanel.c — Dusk Agent Sessions panel with styled rows & friendly empty state. */
#include "sessionspanel.h"

struct BtSessionsPanel {
  GtkWidget *root;
  GtkWidget *stack;
  GtkWidget *list_box;
  BtSessionActionFn on_resume;
  BtSessionActionFn on_rollback;
  gpointer user_data;
};

static void on_resume_clicked(GtkButton *btn, gpointer ud)
{
  BtSessionsPanel *p = ud;
  const gchar *sid = g_object_get_data(G_OBJECT(btn), "bt-session-id");
  if (sid != NULL && p->on_resume != NULL)
    p->on_resume(sid, p->user_data);
}

static void on_rollback_clicked(GtkButton *btn, gpointer ud)
{
  BtSessionsPanel *p = ud;
  const gchar *sid = g_object_get_data(G_OBJECT(btn), "bt-session-id");
  if (sid != NULL && p->on_rollback != NULL)
    p->on_rollback(sid, p->user_data);
}

BtSessionsPanel *bt_sessionspanel_new(BtSessionActionFn on_resume, BtSessionActionFn on_rollback,
                                      gpointer user_data)
{
  BtSessionsPanel *p = g_new0(BtSessionsPanel, 1);
  p->on_resume = on_resume;
  p->on_rollback = on_rollback;
  p->user_data = user_data;

  p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(p->root, "bt-sessions-box");
  gtk_widget_set_hexpand(p->root, TRUE);
  gtk_widget_set_vexpand(p->root, TRUE);
  gtk_widget_set_size_request(p->root, 200, -1);

  GtkWidget *hdr_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr_bar, "bt-card-header");
  GtkWidget *hdr = gtk_label_new("AGENT SESSIONS");
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
  gtk_widget_add_css_class(hdr, "bt-card-title");
  gtk_box_append(GTK_BOX(hdr_bar), hdr);
  gtk_box_append(GTK_BOX(p->root), hdr_bar);

  p->list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list_box), GTK_SELECTION_NONE);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), p->list_box);

  GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(empty, "bt-empty");
  gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
  GtkWidget *eico = gtk_image_new_from_icon_name("bt-sessions-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(eico), 36);
  GtkWidget *etitle = gtk_label_new("No sessions yet");
  gtk_widget_add_css_class(etitle, "bt-empty-title");
  GtkWidget *esub = gtk_label_new("Launch Claude, Codex, or Gemini from the Agent menu");
  gtk_label_set_wrap(GTK_LABEL(esub), TRUE);
  gtk_label_set_justify(GTK_LABEL(esub), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(empty), eico);
  gtk_box_append(GTK_BOX(empty), etitle);
  gtk_box_append(GTK_BOX(empty), esub);

  p->stack = gtk_stack_new();
  gtk_widget_set_hexpand(p->stack, TRUE);
  gtk_widget_set_vexpand(p->stack, TRUE);
  gtk_stack_add_named(GTK_STACK(p->stack), scrolled, "list");
  gtk_stack_add_named(GTK_STACK(p->stack), empty, "empty");
  gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "empty");
  gtk_box_append(GTK_BOX(p->root), p->stack);

  return p;
}

void bt_sessionspanel_free(BtSessionsPanel *panel)
{
  g_free(panel);
}

GtkWidget *bt_sessionspanel_widget(BtSessionsPanel *panel)
{
  return panel->root;
}

void bt_sessionspanel_refresh(BtSessionsPanel *panel, const BtSessionStore *store)
{
  g_return_if_fail(panel != NULL);
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(panel->list_box)) != NULL)
    gtk_list_box_remove(GTK_LIST_BOX(panel->list_box), child);

  if (store == NULL) {
    gtk_stack_set_visible_child_name(GTK_STACK(panel->stack), "empty");
    return;
  }
  const GPtrArray *items = bt_sessions_list(store);
  if (items == NULL || items->len == 0) {
    gtk_stack_set_visible_child_name(GTK_STACK(panel->stack), "empty");
    return;
  }

  for (gint i = (gint)items->len - 1; i >= 0; i--) {
    const BtSession *s = g_ptr_array_index(items, (guint)i);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(vbox, "bt-session-row");

    g_autofree gchar *leaf = g_path_get_basename(s->cwd);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ico = gtk_image_new_from_icon_name(
        g_strcmp0(s->tool, "shell") == 0 ? "bt-sessions-symbolic" : "bt-agent-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ico), 16);
    GtkWidget *lbl = gtk_label_new(s->tool);
    gtk_widget_add_css_class(lbl, "bt-session-title");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_hexpand(lbl, TRUE);

    g_autofree gchar *meta_s = g_strdup_printf("%s · %s", leaf,
                                               s->ended > 0 ? "ended" : "active");
    GtkWidget *meta = gtk_label_new(meta_s);
    gtk_widget_add_css_class(meta, "bt-session-meta");
    gtk_box_append(GTK_BOX(top), ico);
    gtk_box_append(GTK_BOX(top), lbl);
    gtk_box_append(GTK_BOX(top), meta);
    gtk_box_append(GTK_BOX(vbox), top);

    gboolean has_resume = (s->resume_cmd != NULL && s->resume_cmd[0] != '\0');
    gboolean has_rb = (s->checkpoint_oid[0] != '\0');
    if (has_resume || has_rb) {
      GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_margin_top(btns, 4);
      if (has_resume) {
        GtkWidget *res = gtk_button_new_with_label("Resume");
        gtk_widget_add_css_class(res, "bt-hairline");
        g_object_set_data_full(G_OBJECT(res), "bt-session-id", g_strdup(s->id), g_free);
        g_signal_connect(res, "clicked", G_CALLBACK(on_resume_clicked), panel);
        gtk_box_append(GTK_BOX(btns), res);
      }
      if (has_rb) {
        GtkWidget *rb = gtk_button_new_with_label("Rollback");
        gtk_widget_add_css_class(rb, "bt-hairline");
        g_object_set_data_full(G_OBJECT(rb), "bt-session-id", g_strdup(s->id), g_free);
        g_signal_connect(rb, "clicked", G_CALLBACK(on_rollback_clicked), panel);
        gtk_box_append(GTK_BOX(btns), rb);
      }
      gtk_box_append(GTK_BOX(vbox), btns);
    }
    gtk_list_box_append(GTK_LIST_BOX(panel->list_box), vbox);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(panel->stack), "list");
}
