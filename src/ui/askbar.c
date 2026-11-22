/* askbar.c — Ctrl+E unified Search / Ask (evidence pane) / Command bar (§6.B4, P6). */
#include "askbar.h"
#include "search.h"

#include <string.h>

typedef struct {
  GtkWindow *dialog;
  GtkWidget *entry;
  GtkWidget *mode_label;
  GtkWidget *list_box;
  GtkWidget *agent_box;
  GtkWidget *cmd_preview;
  BtIndex *index;
  GPtrArray *last_hits;
  gchar *pending_agent_cmd;

  BtAskOpenNoteFn on_open_note;
  BtAskRunActionFn on_run_action;
  BtAskSendAgentFn on_send_agent;
  gpointer user_data;
} AskCtx;

static void clear_list(GtkWidget *list_box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(list_box)) != NULL)
    gtk_list_box_remove(GTK_LIST_BOX(list_box), child);
}

static void populate(AskCtx *ctx, const gchar *raw)
{
  clear_list(ctx->list_box);
  g_clear_pointer(&ctx->last_hits, g_ptr_array_unref);
  g_clear_pointer(&ctx->pending_agent_cmd, g_free);
  gtk_widget_set_visible(ctx->agent_box, FALSE);

  const gchar *q = NULL;
  BtAskMode mode = bt_search_detect_mode(raw, &q);

  if (mode == BT_ASK_MODE_COMMAND) {
    gtk_label_set_text(GTK_LABEL(ctx->mode_label), "Mode: Command");
    GPtrArray *cmds = bt_search_match_commands(q);
    for (guint i = 0; i < cmds->len; i++) {
      const BtCommandSpec *cs = g_ptr_array_index(cmds, i);
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_margin_start(box, 8);
      gtk_widget_set_margin_end(box, 8);
      gtk_widget_set_margin_top(box, 4);
      gtk_widget_set_margin_bottom(box, 4);
      GtkWidget *l1 = gtk_label_new(cs->label);
      gtk_label_set_xalign(GTK_LABEL(l1), 0.0f);
      GtkWidget *l2 = gtk_label_new(cs->description);
      gtk_label_set_xalign(GTK_LABEL(l2), 0.0f);
      gtk_widget_add_css_class(l2, "dim-label");
      gtk_box_append(GTK_BOX(box), l1);
      gtk_box_append(GTK_BOX(box), l2);
      GtkWidget *row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
      g_object_set_data(G_OBJECT(row), "bt-action", (gpointer)cs->action_name);
      gtk_list_box_append(GTK_LIST_BOX(ctx->list_box), row);
    }
    g_ptr_array_unref(cmds);
    return;
  }

  gtk_label_set_text(GTK_LABEL(ctx->mode_label),
                     mode == BT_ASK_MODE_ASK ? "Mode: Ask (Evidence — no chat UI)" : "Mode: Search (FTS5)");
  if (ctx->index == NULL || q[0] == '\0')
    return;

  ctx->last_hits = bt_index_search_fts(ctx->index, q, 25, NULL);
  for (guint i = 0; ctx->last_hits != NULL && i < ctx->last_hits->len; i++) {
    const BtSearchHit *h = g_ptr_array_index(ctx->last_hits, i);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);

    gchar *hdr = g_strdup_printf("%s  (%s)", h->title, h->path);
    GtkWidget *l1 = gtk_label_new(hdr);
    gtk_label_set_xalign(GTK_LABEL(l1), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l1), PANGO_ELLIPSIZE_END);
    g_free(hdr);

    GtkWidget *l2 = gtk_label_new(h->snippet);
    gtk_label_set_xalign(GTK_LABEL(l2), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(l2), TRUE);
    gtk_widget_add_css_class(l2, "dim-label");

    gtk_box_append(GTK_BOX(box), l1);
    gtk_box_append(GTK_BOX(box), l2);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "bt-path", g_strdup(h->path), g_free);
    gtk_list_box_append(GTK_LIST_BOX(ctx->list_box), row);
  }

  if (mode == BT_ASK_MODE_ASK) {
    /* §6.B4 & §8.1: show exact command preview before writing to terminal. */
    ctx->pending_agent_cmd = bt_search_compose_ask_agent_cmd(q, ctx->last_hits, 5);
    gchar *disp = g_strdup(ctx->pending_agent_cmd);
    g_strstrip(disp);
    gtk_label_set_text(GTK_LABEL(ctx->cmd_preview), disp);
    g_free(disp);
    gtk_widget_set_visible(ctx->agent_box, TRUE);
  }
}

static void on_changed(GtkSearchEntry *entry, gpointer ud)
{
  populate(ud, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

static void activate_row(AskCtx *ctx, GtkListBoxRow *row)
{
  if (row == NULL)
    return;
  const gchar *act = g_object_get_data(G_OBJECT(row), "bt-action");
  const gchar *path = g_object_get_data(G_OBJECT(row), "bt-path");
  if (act != NULL && ctx->on_run_action != NULL) {
    BtAskRunActionFn fn = ctx->on_run_action;
    gpointer ud = ctx->user_data;
    gtk_window_destroy(ctx->dialog);
    fn(act, ud);
  } else if (path != NULL && ctx->on_open_note != NULL) {
    gchar *copy = g_strdup(path);
    BtAskOpenNoteFn fn = ctx->on_open_note;
    gpointer ud = ctx->user_data;
    gtk_window_destroy(ctx->dialog);
    fn(copy, ud);
    g_free(copy);
  }
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
  (void)lb;
  activate_row(ud, row);
}

static void on_entry_activate(GtkSearchEntry *entry, gpointer ud)
{
  (void)entry;
  AskCtx *ctx = ud;
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ctx->list_box), 0);
  activate_row(ctx, row);
}

static void on_open_in_agent(GtkButton *btn, gpointer ud)
{
  (void)btn;
  AskCtx *ctx = ud;
  if (ctx->pending_agent_cmd != NULL && ctx->on_send_agent != NULL) {
    gchar *cmd = g_strdup(ctx->pending_agent_cmd);
    BtAskSendAgentFn fn = ctx->on_send_agent;
    gpointer data = ctx->user_data;
    gtk_window_destroy(ctx->dialog);
    fn(cmd, data);
    g_free(cmd);
  }
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint kc, GdkModifierType st,
                       gpointer ud)
{
  (void)c; (void)kc; (void)st;
  AskCtx *ctx = ud;
  if (keyval == GDK_KEY_Escape) {
    gtk_window_destroy(ctx->dialog);
    return TRUE;
  }
  return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer ud)
{
  (void)w;
  AskCtx *ctx = ud;
  g_clear_pointer(&ctx->last_hits, g_ptr_array_unref);
  g_free(ctx->pending_agent_cmd);
  g_free(ctx);
}

void bt_askbar_show(GtkWindow *parent, BtIndex *index, BtAskOpenNoteFn on_open_note,
                    BtAskRunActionFn on_run_action, BtAskSendAgentFn on_send_agent,
                    gpointer user_data)
{
  AskCtx *ctx = g_new0(AskCtx, 1);
  ctx->index = index;
  ctx->on_open_note = on_open_note;
  ctx->on_run_action = on_run_action;
  ctx->on_send_agent = on_send_agent;
  ctx->user_data = user_data;

  ctx->dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(ctx->dialog, "Ask / Search / Command (Ctrl+E)");
  gtk_window_set_transient_for(ctx->dialog, parent);
  gtk_window_set_modal(ctx->dialog, TRUE);
  gtk_window_set_default_size(ctx->dialog, 640, 460);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vbox, 10);
  gtk_widget_set_margin_bottom(vbox, 10);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);

  ctx->entry = gtk_search_entry_new();
  gtk_box_append(GTK_BOX(vbox), ctx->entry);

  ctx->mode_label = gtk_label_new("Mode: Search (FTS5) — prefix '>' for commands, '?' for Ask");
  gtk_label_set_xalign(GTK_LABEL(ctx->mode_label), 0.0f);
  gtk_widget_add_css_class(ctx->mode_label, "dim-label");
  gtk_box_append(GTK_BOX(vbox), ctx->mode_label);

  ctx->list_box = gtk_list_box_new();
  g_signal_connect(ctx->list_box, "row-activated", G_CALLBACK(on_row_activated), ctx);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), ctx->list_box);
  gtk_box_append(GTK_BOX(vbox), scrolled);

  /* Evidence -> Open in agent bar (P6: no chat UI, command preview shown). */
  ctx->agent_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  ctx->cmd_preview = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(ctx->cmd_preview), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(ctx->cmd_preview), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(ctx->cmd_preview, TRUE);
  GtkWidget *agent_btn = gtk_button_new_with_label("Open in agent");
  g_signal_connect(agent_btn, "clicked", G_CALLBACK(on_open_in_agent), ctx);
  gtk_box_append(GTK_BOX(ctx->agent_box), ctx->cmd_preview);
  gtk_box_append(GTK_BOX(ctx->agent_box), agent_btn);
  gtk_widget_set_visible(ctx->agent_box, FALSE);
  gtk_box_append(GTK_BOX(vbox), ctx->agent_box);

  gtk_window_set_child(ctx->dialog, vbox);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), ctx);
  gtk_widget_add_controller(GTK_WIDGET(ctx->dialog), key);
  g_signal_connect(ctx->entry, "search-changed", G_CALLBACK(on_changed), ctx);
  g_signal_connect(ctx->entry, "activate", G_CALLBACK(on_entry_activate), ctx);
  g_signal_connect(ctx->dialog, "destroy", G_CALLBACK(on_destroy), ctx);

  gtk_window_present(ctx->dialog);
  gtk_widget_grab_focus(ctx->entry);
}
