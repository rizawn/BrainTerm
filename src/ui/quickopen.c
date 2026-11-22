/* quickopen.c — Ctrl+P fuzzy note picker (§6.A2). */
#include "quickopen.h"
#include "noteutil.h"

#include <string.h>

#define BT_QUICKOPEN_MAX 100u

typedef struct {
  const gchar *relpath;
  gint score;
} ScoredNote;

typedef struct {
  GtkWindow *dialog;
  GtkWidget *entry;
  GtkStringList *model;
  GtkSingleSelection *selection;
  GPtrArray *all_notes; /* owned copies of relpaths */
  BtQuickOpenFn on_pick;
  gpointer user_data;
} QuickOpenCtx;

static gint cmp_scored(const void *a, const void *b)
{
  const ScoredNote *sa = a;
  const ScoredNote *sb = b;
  if (sa->score != sb->score)
    return sb->score - sa->score;
  return strcmp(sa->relpath, sb->relpath);
}

static void refilter(QuickOpenCtx *ctx, const gchar *query)
{
  guint n = ctx->all_notes->len;
  GArray *hits = g_array_sized_new(FALSE, FALSE, sizeof(ScoredNote), MIN(n, 256u));
  for (guint i = 0; i < n; i++) {
    const gchar *rel = g_ptr_array_index(ctx->all_notes, i);
    gint s = bt_fuzzy_score(query, rel);
    if (s >= 0) {
      ScoredNote sn = {rel, s};
      g_array_append_val(hits, sn);
    }
  }
  g_array_sort(hits, cmp_scored);

  guint keep = MIN(hits->len, BT_QUICKOPEN_MAX);
  const gchar **items = g_new(const gchar *, keep + 1);
  for (guint i = 0; i < keep; i++)
    items[i] = g_array_index(hits, ScoredNote, i).relpath;
  items[keep] = NULL;

  guint old_n = g_list_model_get_n_items(G_LIST_MODEL(ctx->model));
  gtk_string_list_splice(ctx->model, 0, old_n, items);
  if (keep > 0)
    gtk_single_selection_set_selected(ctx->selection, 0);

  g_free(items);
  g_array_free(hits, TRUE);
}

static void pick_selected(QuickOpenCtx *ctx)
{
  GtkStringObject *obj = GTK_STRING_OBJECT(gtk_single_selection_get_selected_item(ctx->selection));
  if (obj != NULL && ctx->on_pick != NULL) {
    gchar *rel = g_strdup(gtk_string_object_get_string(obj));
    BtQuickOpenFn fn = ctx->on_pick;
    gpointer ud = ctx->user_data;
    gtk_window_destroy(ctx->dialog);
    fn(rel, ud);
    g_free(rel);
  }
}

static void on_search_changed(GtkSearchEntry *entry, gpointer ud)
{
  refilter(ud, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

static void on_entry_activate(GtkSearchEntry *entry, gpointer ud)
{
  (void)entry;
  pick_selected(ud);
}

static void on_list_activate(GtkListView *view, guint pos, gpointer ud)
{
  (void)view;
  QuickOpenCtx *ctx = ud;
  gtk_single_selection_set_selected(ctx->selection, pos);
  pick_selected(ctx);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                               GdkModifierType state, gpointer ud)
{
  (void)ctrl;
  (void)keycode;
  (void)state;
  QuickOpenCtx *ctx = ud;
  if (keyval == GDK_KEY_Escape) {
    gtk_window_destroy(ctx->dialog);
    return TRUE;
  }
  guint cur = gtk_single_selection_get_selected(ctx->selection);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->model));
  if (n == 0)
    return FALSE;
  if (keyval == GDK_KEY_Down) {
    gtk_single_selection_set_selected(ctx->selection, cur + 1 < n ? cur + 1 : 0);
    return TRUE;
  }
  if (keyval == GDK_KEY_Up) {
    gtk_single_selection_set_selected(ctx->selection, cur > 0 ? cur - 1 : n - 1);
    return TRUE;
  }
  return FALSE;
}

static void on_factory_setup(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f;
  (void)ud;
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
  gtk_widget_set_margin_start(label, 8);
  gtk_widget_set_margin_end(label, 8);
  gtk_widget_set_margin_top(label, 4);
  gtk_widget_set_margin_bottom(label, 4);
  gtk_list_item_set_child(li, label);
}

static void on_factory_bind(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
  (void)f;
  (void)ud;
  GtkWidget *label = gtk_list_item_get_child(li);
  GtkStringObject *obj = GTK_STRING_OBJECT(gtk_list_item_get_item(li));
  gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(obj));
}

static void on_dialog_destroy(GtkWidget *w, gpointer ud)
{
  (void)w;
  QuickOpenCtx *ctx = ud;
  g_ptr_array_unref(ctx->all_notes);
  g_clear_object(&ctx->selection);
  g_clear_object(&ctx->model);
  g_free(ctx);
}

void bt_quickopen_show(GtkWindow *parent, const BtVault *vault, BtQuickOpenFn on_pick,
                       gpointer user_data)
{
  if (vault == NULL)
    return;
  QuickOpenCtx *ctx = g_new0(QuickOpenCtx, 1);
  ctx->on_pick = on_pick;
  ctx->user_data = user_data;
  ctx->all_notes = g_ptr_array_new_with_free_func(g_free);
  const GPtrArray *notes = vault_notes(vault);
  for (guint i = 0; i < notes->len; i++)
    g_ptr_array_add(ctx->all_notes, g_strdup(g_ptr_array_index(notes, i)));

  ctx->dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(ctx->dialog, "Quick Open Note");
  gtk_window_set_transient_for(ctx->dialog, parent);
  gtk_window_set_modal(ctx->dialog, TRUE);
  gtk_window_set_default_size(ctx->dialog, 520, 380);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);
  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);

  ctx->entry = gtk_search_entry_new();
  gtk_box_append(GTK_BOX(vbox), ctx->entry);

  ctx->model = gtk_string_list_new(NULL);
  ctx->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(ctx->model)));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), NULL);

  GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(ctx->selection)), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list), TRUE);
  g_signal_connect(list, "activate", G_CALLBACK(on_list_activate), ctx);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
  gtk_box_append(GTK_BOX(vbox), scrolled);

  gtk_window_set_child(ctx->dialog, vbox);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), ctx);
  gtk_widget_add_controller(GTK_WIDGET(ctx->dialog), key);

  g_signal_connect(ctx->entry, "search-changed", G_CALLBACK(on_search_changed), ctx);
  g_signal_connect(ctx->entry, "activate", G_CALLBACK(on_entry_activate), ctx);
  g_signal_connect(ctx->dialog, "destroy", G_CALLBACK(on_dialog_destroy), ctx);

  refilter(ctx, "");
  gtk_window_present(ctx->dialog);
  gtk_widget_grab_focus(ctx->entry);
}
