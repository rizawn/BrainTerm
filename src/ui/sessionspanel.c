/* sessionspanel.c — GtkListBox for recorded agent sessions (§6.D6, §6.D7). */
#include "sessionspanel.h"

struct BtSessionsPanel {
  GtkWidget *root;
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

  p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_name(p->root, "bt-sessions-box");
  gtk_widget_set_hexpand(p->root, TRUE);
  gtk_widget_set_vexpand(p->root, TRUE);
  gtk_widget_set_size_request(p->root, 240, -1);

  GtkWidget *hdr = gtk_label_new("Agent Sessions");
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
  gtk_widget_set_margin_start(hdr, 8);
  gtk_widget_set_margin_top(hdr, 6);
  gtk_widget_add_css_class(hdr, "heading");
  gtk_box_append(GTK_BOX(p->root), hdr);

  p->list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list_box), GTK_SELECTION_NONE);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), p->list_box);
  gtk_box_append(GTK_BOX(p->root), scrolled);

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

  if (store == NULL)
    return;
  const GPtrArray *items = bt_sessions_list(store);
  for (gint i = (gint)items->len - 1; i >= 0; i--) {
    const BtSession *s = g_ptr_array_index(items, (guint)i);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 6);
    gtk_widget_set_margin_bottom(vbox, 6);

    gchar *leaf = g_path_get_basename(s->cwd);
    gchar *title = g_strdup_printf("%s  (%s)  [%s]", s->tool, leaf,
                                   s->ended > 0 ? "ended" : "active/aborted");
    GtkWidget *lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    g_free(title);
    g_free(leaf);
    gtk_box_append(GTK_BOX(vbox), lbl);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    if (s->resume_cmd != NULL && s->resume_cmd[0] != '\0') {
      GtkWidget *res = gtk_button_new_with_label("Resume");
      gtk_widget_add_css_class(res, "bt-hairline");
      g_object_set_data_full(G_OBJECT(res), "bt-session-id", g_strdup(s->id), g_free);
      g_signal_connect(res, "clicked", G_CALLBACK(on_resume_clicked), panel);
      gtk_box_append(GTK_BOX(btns), res);
    }
    if (s->checkpoint_oid[0] != '\0') {
      GtkWidget *rb = gtk_button_new_with_label("Rollback");
      gtk_widget_add_css_class(rb, "bt-hairline");
      g_object_set_data_full(G_OBJECT(rb), "bt-session-id", g_strdup(s->id), g_free);
      g_signal_connect(rb, "clicked", G_CALLBACK(on_rollback_clicked), panel);
      gtk_box_append(GTK_BOX(btns), rb);
    }
    gtk_box_append(GTK_BOX(vbox), btns);
    gtk_list_box_append(GTK_LIST_BOX(panel->list_box), vbox);
  }
}
