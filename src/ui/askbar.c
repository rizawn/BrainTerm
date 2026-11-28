/* askbar.c — Ctrl+E unified Search / Ask (evidence pane) / Command bar (§6.B4, P6). */
#include "askbar.h"
#include "embed.h"
#include "search.h"

#include <string.h>

#define BT_ASK_HISTORY_CAP 10

static gchar *g_search_history[BT_ASK_HISTORY_CAP];
static gint g_search_history_len = 0;

static void record_search_history(const gchar *raw)
{
  if (raw == NULL || raw[0] == '\0')
    return;
  if (g_search_history_len > 0 && strcmp(g_search_history[0], raw) == 0)
    return;
  g_free(g_search_history[BT_ASK_HISTORY_CAP - 1]);
  for (gint i = MIN(g_search_history_len, BT_ASK_HISTORY_CAP - 1); i > 0; i--)
    g_search_history[i] = g_search_history[i - 1];
  g_search_history[0] = g_strdup(raw);
  if (g_search_history_len < BT_ASK_HISTORY_CAP)
    g_search_history_len++;
}

typedef struct {
  GtkWindow *dialog;
  GtkWidget *entry;
  GtkWidget *mode_label;
  GtkWidget *hybrid_btn;
  GtkWidget *list_box;
  GtkWidget *agent_box;
  GtkWidget *cmd_preview;
  const BtVault *vault;
  BtIndex *index;
  gchar *agent_cli;
  gboolean hybrid_enabled;
  gint hist_idx;
  GPtrArray *last_hits;
  gchar *pending_agent_cmd;

  BtAskHybridToggleFn on_hybrid_toggle;
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

  guint cached_vecs = ctx->index ? bt_embed_count_cached(ctx->index) : 0u;
  gboolean can_hybrid = (ctx->hybrid_enabled && (cached_vecs > 0u || ctx->vault != NULL));
  g_autofree gchar *mode_txt = NULL;
  if (mode == BT_ASK_MODE_ASK)
    mode_txt = g_strdup_printf("Mode: Ask (%s — agent: %s)",
                               can_hybrid ? "Hybrid FTS5+Vector" : "FTS5 Evidence",
                               ctx->agent_cli ? ctx->agent_cli : "claude");
  else
    mode_txt = g_strdup_printf("Mode: Search (%s)",
                               can_hybrid ? "Hybrid FTS5 + Local Vector RRF" : "FTS5 Keyword");
  gtk_label_set_text(GTK_LABEL(ctx->mode_label), mode_txt);
  if (ctx->index == NULL || q[0] == '\0')
    return;

  ctx->last_hits = can_hybrid
                       ? bt_embed_hybrid_search(ctx->vault, ctx->index, q, 25, TRUE, NULL)
                       : bt_index_search_fts(ctx->index, q, 25, NULL);
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
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ctx->list_box), 0);
  if (first != NULL)
    gtk_list_box_select_row(GTK_LIST_BOX(ctx->list_box), first);

  if (mode == BT_ASK_MODE_ASK) {
    /* §6.B4 & §8.1: show exact command preview before writing to terminal. */
    ctx->pending_agent_cmd = bt_search_compose_ask_agent_cmd_for(
        ctx->agent_cli, q, ctx->last_hits, 5);
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
  record_search_history(gtk_editable_get_text(GTK_EDITABLE(ctx->entry)));
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
  GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ctx->list_box));
  if (row == NULL)
    row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ctx->list_box), 0);
  activate_row(ctx, row);
}

static void on_open_in_agent(GtkButton *btn, gpointer ud)
{
  (void)btn;
  AskCtx *ctx = ud;
  if (ctx->pending_agent_cmd != NULL && ctx->on_send_agent != NULL) {
    record_search_history(gtk_editable_get_text(GTK_EDITABLE(ctx->entry)));
    gchar *cmd = g_strdup(ctx->pending_agent_cmd);
    BtAskSendAgentFn fn = ctx->on_send_agent;
    gpointer data = ctx->user_data;
    gtk_window_destroy(ctx->dialog);
    fn(cmd, data);
    g_free(cmd);
  }
}

static void on_hybrid_toggled(GtkToggleButton *btn, gpointer ud)
{
  AskCtx *ctx = ud;
  ctx->hybrid_enabled = gtk_toggle_button_get_active(btn);
  if (ctx->on_hybrid_toggle != NULL)
    ctx->on_hybrid_toggle(ctx->hybrid_enabled, ctx->user_data);
  populate(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->entry)));
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint kc, GdkModifierType st,
                       gpointer ud)
{
  (void)c; (void)kc;
  AskCtx *ctx = ud;
  if (keyval == GDK_KEY_Escape) {
    gtk_window_destroy(ctx->dialog);
    return TRUE;
  }
  if ((st & GDK_ALT_MASK) != 0 && (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down)) {
    if (g_search_history_len == 0)
      return TRUE;
    if (keyval == GDK_KEY_Up)
      ctx->hist_idx = MIN(ctx->hist_idx + 1, g_search_history_len - 1);
    else
      ctx->hist_idx = MAX(ctx->hist_idx - 1, -1);
    const gchar *txt = (ctx->hist_idx >= 0) ? g_search_history[ctx->hist_idx] : "";
    gtk_editable_set_text(GTK_EDITABLE(ctx->entry), txt);
    gtk_editable_set_position(GTK_EDITABLE(ctx->entry), -1);
    return TRUE;
  }
  if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(ctx->list_box));
    gint idx = sel ? gtk_list_box_row_get_index(sel) : -1;
    gint next_idx = (keyval == GDK_KEY_Down) ? (idx + 1) : MAX(idx - 1, 0);
    GtkListBoxRow *nxt = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ctx->list_box), next_idx);
    if (nxt != NULL)
      gtk_list_box_select_row(GTK_LIST_BOX(ctx->list_box), nxt);
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
  g_free(ctx->agent_cli);
  g_free(ctx);
}

void bt_askbar_show_ex(GtkWindow *parent, const BtVault *vault, BtIndex *index,
                       const gchar *agent_cli, gboolean hybrid_enabled,
                       BtAskHybridToggleFn on_hybrid_toggle,
                       BtAskOpenNoteFn on_open_note, BtAskRunActionFn on_run_action,
                       BtAskSendAgentFn on_send_agent, gpointer user_data)
{
  AskCtx *ctx = g_new0(AskCtx, 1);
  ctx->vault = vault;
  ctx->index = index;
  ctx->agent_cli = g_strdup((agent_cli && agent_cli[0]) ? agent_cli : "claude");
  ctx->hybrid_enabled = hybrid_enabled;
  ctx->hist_idx = -1;
  ctx->on_hybrid_toggle = on_hybrid_toggle;
  ctx->on_open_note = on_open_note;
  ctx->on_run_action = on_run_action;
  ctx->on_send_agent = on_send_agent;
  ctx->user_data = user_data;

  ctx->dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(ctx->dialog, "Ask / Search / Command (Ctrl+E)");
  gtk_window_set_transient_for(ctx->dialog, parent);
  gtk_window_set_modal(ctx->dialog, TRUE);
  gtk_window_set_default_size(ctx->dialog, 660, 480);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vbox, 10);
  gtk_widget_set_margin_bottom(vbox, 10);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);

  ctx->entry = gtk_search_entry_new();
  gtk_accessible_update_property(GTK_ACCESSIBLE(ctx->entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Search notes, run >command, or ask ?question", -1);
  gtk_box_append(GTK_BOX(vbox), ctx->entry);

  GtkWidget *meta_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  ctx->mode_label = gtk_label_new("Mode: Search — prefix '>' for commands, '?' for Ask (Alt+Up history)");
  gtk_label_set_xalign(GTK_LABEL(ctx->mode_label), 0.0f);
  gtk_widget_set_hexpand(ctx->mode_label, TRUE);
  gtk_widget_add_css_class(ctx->mode_label, "dim-label");

  guint cached = index ? bt_embed_count_cached(index) : 0u;
  g_autofree gchar *hyb_lbl = cached > 0u ? g_strdup_printf("Hybrid (%u vecs)", cached)
                                          : g_strdup("Hybrid");
  ctx->hybrid_btn = gtk_toggle_button_new_with_label(hyb_lbl);
  gtk_widget_add_css_class(ctx->hybrid_btn, "bt-hairline");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->hybrid_btn), hybrid_enabled);
  gtk_widget_set_tooltip_text(ctx->hybrid_btn,
                              cached > 0u ? "Blend FTS5 + cached local vector similarity"
                                          : "Blend FTS5 + local vector similarity (sync embeddings in Preferences for instant RAM lookup)");
  g_signal_connect(ctx->hybrid_btn, "toggled", G_CALLBACK(on_hybrid_toggled), ctx);
  gtk_box_append(GTK_BOX(meta_row), ctx->mode_label);
  gtk_box_append(GTK_BOX(meta_row), ctx->hybrid_btn);
  gtk_box_append(GTK_BOX(vbox), meta_row);

  ctx->list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(ctx->list_box), GTK_SELECTION_SINGLE);
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
  gtk_widget_add_css_class(agent_btn, "suggested-action");
  g_signal_connect(agent_btn, "clicked", G_CALLBACK(on_open_in_agent), ctx);
  gtk_box_append(GTK_BOX(ctx->agent_box), ctx->cmd_preview);
  gtk_box_append(GTK_BOX(ctx->agent_box), agent_btn);
  gtk_widget_set_visible(ctx->agent_box, FALSE);
  gtk_box_append(GTK_BOX(vbox), ctx->agent_box);

  gtk_window_set_child(ctx->dialog, vbox);

  GtkEventController *key = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), ctx);
  gtk_widget_add_controller(GTK_WIDGET(ctx->dialog), key);
  g_signal_connect(ctx->entry, "search-changed", G_CALLBACK(on_changed), ctx);
  g_signal_connect(ctx->entry, "activate", G_CALLBACK(on_entry_activate), ctx);
  g_signal_connect(ctx->dialog, "destroy", G_CALLBACK(on_destroy), ctx);

  gtk_window_present(ctx->dialog);
  gtk_widget_grab_focus(ctx->entry);
}

void bt_askbar_show(GtkWindow *parent, BtIndex *index, BtAskOpenNoteFn on_open_note,
                    BtAskRunActionFn on_run_action, BtAskSendAgentFn on_send_agent,
                    gpointer user_data)
{
  bt_askbar_show_ex(parent, NULL, index, "claude", FALSE, NULL,
                    on_open_note, on_run_action, on_send_agent, user_data);
}
