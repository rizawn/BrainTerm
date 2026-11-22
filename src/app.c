/* app.c — BtApp with libadwaita Ink reskin (ADR-009, M-UI).
 * Presentation-layer reskin only; all M1–M8 actions & core contracts intact. */
#include "app.h"
#include "agents.h"
#include "checkpoint.h"
#include "frontmatter.h"
#include "graph.h"
#include "graphview.h"
#include "index.h"
#include "noteutil.h"
#include "organizer.h"
#include "sessions.h"
#include "theme.h"
#include "ui/askbar.h"
#include "ui/editorpane.h"
#include "ui/filetree.h"
#include "ui/quickopen.h"
#include "ui/sessionspanel.h"
#include "ui/termtabs.h"
#include "ui/themeload.h"
#include "watcher.h"

#include <adwaita.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <string.h>

struct BtApp {
  AdwApplication *adw_app;
  GtkWindow *window;
  AdwToastOverlay *toast_overlay;
  AdwOverlaySplitView *split_view;
  AdwViewStack *view_stack;
  AdwWindowTitle *win_title;
  AdwSplitButton *agent_split;
  GtkWidget *sidebar_vault_lbl;
  GtkWidget *top_paned;
  GtkWidget *right_paned;
  GtkWidget *graph_toggle;
  BtThemeLoader *themeloader;

  BtVault *vault;
  BtIndex *index;
  BtWatcher *watcher;
  BtFileTree *tree;
  BtEditorPane *editor;
  BtGraphView *graphview;
  BtTermTabs *tabs;
  BtSessionsPanel *sessions_panel;
  BtSessionStore *sessions;
  BtAgentRegistry agents;

  BtUiScheme scheme;
  gboolean graph_visible;
  gboolean syncing_graph_toggle;
  gboolean term_fullscreen;
  gboolean allow_launch_without_checkpoint; /* R10: FALSE by default */

  gchar *cli_vault;
  gboolean smoke_test;
};

/* ----------------------------------------------------- state.ini (§7.5) -- */

gchar *bt_app_state_file_path(void)
{
  const gchar *cfg = g_getenv("BT_CONFIG_DIR");
  if (cfg == NULL || cfg[0] == '\0')
    cfg = g_get_user_config_dir();
  return g_build_filename(cfg, "brainterm", "state.ini", NULL);
}

gchar *bt_app_load_last_vault(void)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  gchar *last = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    last = g_key_file_get_string(kf, "vault", "last_path", NULL);
  g_key_file_unref(kf);
  g_free(path);
  return last;
}

static gboolean save_state_str(const gchar *group, const gchar *key, const gchar *value,
                               GError **error)
{
  gchar *path = bt_app_state_file_path();
  gchar *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot create config directory '%s'", dir);
    g_free(dir);
    g_free(path);
    return FALSE;
  }
  g_free(dir);

  GKeyFile *kf = g_key_file_new();
  g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
  g_key_file_set_string(kf, group, key, value);

  GError *local = NULL;
  gboolean ok = g_key_file_save_to_file(kf, path, &local);
  if (!ok) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot save state '%s': %s", path, local->message);
    g_error_free(local);
  }
  g_key_file_unref(kf);
  g_free(path);
  return ok;
}

gboolean bt_app_save_last_vault(const gchar *vault_path, GError **error)
{
  return save_state_str("vault", "last_path", vault_path, error);
}

const gchar *bt_app_resolve_page_name(const gchar *saved)
{
  if (saved != NULL) {
    if (strcmp(saved, "graph") == 0)
      return "graph";
    if (strcmp(saved, "sessions") == 0)
      return "sessions";
  }
  return "editor";
}

gchar *bt_app_load_saved_page(void)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  gchar *raw = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    raw = g_key_file_get_string(kf, "ui", "page", NULL);
  const gchar *resolved = bt_app_resolve_page_name(raw);
  g_free(raw);
  g_key_file_unref(kf);
  g_free(path);
  return g_strdup(resolved);
}

void bt_app_save_page(const gchar *page)
{
  save_state_str("ui", "page", bt_app_resolve_page_name(page), NULL);
}

static gboolean load_saved_graph_visible(void)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  gboolean vis = TRUE; /* default: dual-pane Editor (left) + Graph (right) */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    gchar *v = g_key_file_get_string(kf, "ui", "graph_visible", NULL);
    if (v != NULL) {
      vis = (g_strcmp0(v, "0") != 0 && g_ascii_strcasecmp(v, "false") != 0);
      g_free(v);
    }
  }
  g_key_file_unref(kf);
  g_free(path);
  return vis;
}

static void save_graph_visible(gboolean vis)
{
  save_state_str("ui", "graph_visible", vis ? "1" : "0", NULL);
}

static BtUiScheme load_saved_scheme(void)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  BtUiScheme sc = BT_SCHEME_DARK; /* default: dark-first per primary mock */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    gchar *s = g_key_file_get_string(kf, "ui", "scheme", NULL);
    if (s != NULL) {
      sc = bt_scheme_from_string(s);
      g_free(s);
    }
  }
  g_key_file_unref(kf);
  g_free(path);
  return sc;
}

static void save_scheme(BtUiScheme sc)
{
  save_state_str("ui", "scheme", bt_scheme_to_string(sc), NULL);
}

/* ------------------------------------------- Ink theme runtime switch -- */

/* "Dusk" ships a single dark theme (light is not provided yet): every scheme
 * preference resolves to dark so chrome, editor, graph and terminal never mix. */
static gboolean is_effective_dark(BtApp *app)
{
  (void)app;
  return TRUE;
}

static void apply_scheme_runtime(BtApp *app, BtUiScheme sc, gboolean persist)
{
  app->scheme = sc;
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
  if (app->editor != NULL)
    bt_editorpane_set_dark(app->editor, TRUE);
  if (app->graphview != NULL)
    bt_graphview_set_dark(app->graphview, TRUE);
  if (persist && !app->smoke_test)
    save_scheme(sc);
}

static void install_theme(BtApp *app)
{
  if (app->themeloader == NULL)
    app->themeloader = bt_themeloader_new(NULL);
}

/* ----------------------------------------------------------- UI helpers -- */

static void update_header(BtApp *app)
{
  if (app->win_title == NULL)
    return;
  const gchar *note = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
  gboolean dirty = app->editor ? bt_editorpane_is_dirty(app->editor) : FALSE;

  if (app->vault != NULL) {
    gchar *base = g_path_get_basename(vault_root(app->vault));
    adw_window_title_set_title(app->win_title, base);
    gchar *sub = note ? g_strdup_printf("%s %s · %u notes", dirty ? "○" : "●", note,
                                        vault_notes(app->vault)->len)
                      : g_strdup_printf("%u notes", vault_notes(app->vault)->len);
    adw_window_title_set_subtitle(app->win_title, sub);
    if (app->sidebar_vault_lbl != NULL) {
      gchar *sl = g_strdup_printf("● %s", base);
      gtk_label_set_text(GTK_LABEL(app->sidebar_vault_lbl), sl);
      g_free(sl);
    }
    g_free(sub);
    g_free(base);
  } else {
    adw_window_title_set_title(app->win_title, "BrainTerm");
    adw_window_title_set_subtitle(app->win_title, "No vault open");
  }
}

static void show_toast(BtApp *app, const gchar *msg, guint timeout_s)
{
  if (app == NULL || app->toast_overlay == NULL || app->smoke_test || msg == NULL)
    return;
  AdwToast *t = adw_toast_new(msg);
  adw_toast_set_timeout(t, timeout_s);
  adw_toast_overlay_add_toast(app->toast_overlay, t);
}

void bt_app_set_status(BtApp *app, const gchar *message)
{
  show_toast(app, message, 2);
}

void bt_app_show_error(BtApp *app, const gchar *title, const gchar *detail)
{
  if (app == NULL || app->window == NULL || app->smoke_test) {
    g_warning("%s: %s", title, detail ? detail : "");
    return;
  }
  GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", title);
  if (detail != NULL)
    gtk_alert_dialog_set_detail(dlg, detail);
  gtk_alert_dialog_show(dlg, app->window);
  g_object_unref(dlg);
}

static void refresh_graph(BtApp *app, gboolean animate)
{
  if (app->graphview == NULL || app->vault == NULL || app->index == NULL)
    return;
  BtGraph *g = bt_graph_build(app->vault, app->index, app->sessions);
  if (g != NULL) {
    bt_graph_layout(g, 20);
    bt_graphview_set_dark(app->graphview, is_effective_dark(app));
    bt_graphview_set_graph(app->graphview, g, animate);
  }
}

static void on_watcher_event(BtWatchEventKind kind, const gchar *relpath, gpointer ud);

/* --------------------------------------------------------- core actions -- */

gboolean bt_app_open_vault(BtApp *app, const gchar *path, GError **error)
{
  g_return_val_if_fail(app != NULL, FALSE);
  if (app->editor != NULL && bt_editorpane_is_dirty(app->editor))
    bt_editorpane_flush_save(app->editor);

  BtScanStats st = {0};
  BtVault *v = vault_open(path, &st, error);
  if (v == NULL)
    return FALSE;

  if (app->tabs != NULL)
    bt_termtabs_close_all(app->tabs);
  if (app->sessions != NULL) {
    bt_sessions_free(app->sessions);
    app->sessions = NULL;
  }
  if (app->watcher != NULL) {
    bt_watcher_free(app->watcher);
    app->watcher = NULL;
  }
  if (app->index != NULL) {
    bt_index_close(app->index);
    app->index = NULL;
  }
  if (app->vault != NULL)
    vault_free(app->vault);
  app->vault = v;

  if (!app->smoke_test) {
    gboolean differs = FALSE;
    bt_agents_apply_vault_template(app->vault, &differs, NULL);
    bt_app_save_last_vault(vault_root(app->vault), NULL);
  }

  gboolean r2_rebuilt = FALSE;
  GError *ierr = NULL;
  app->index = bt_index_open(vault_root(app->vault), &r2_rebuilt, &ierr);
  if (app->index != NULL) {
    bt_index_rebuild(app->index, app->vault, NULL);
    bt_index_import_sessions_json(app->index, vault_root(app->vault), NULL);
  } else if (ierr != NULL) {
    bt_app_show_error(app, "Index error", ierr->message);
    g_error_free(ierr);
  }
  app->watcher = bt_watcher_new(app->vault, BT_WATCHER_DEBOUNCE_MS, on_watcher_event, app);

  app->sessions = bt_sessions_open(vault_root(app->vault), NULL);
  if (app->sessions_panel != NULL)
    bt_sessionspanel_refresh(app->sessions_panel, app->sessions);

  if (app->editor != NULL)
    bt_editorpane_clear(app->editor);
  if (app->tree != NULL)
    bt_filetree_populate(app->tree, app->vault);
  refresh_graph(app, !app->smoke_test);

  if (app->tabs != NULL) {
    GError *terr = NULL;
    const BtSession *sess = bt_sessions_add(app->sessions, "shell", vault_root(app->vault), "",
                                            NULL, NULL, NULL);
    if (!bt_termtabs_open(app->tabs, app->vault, NULL, NULL, sess ? sess->id : NULL, &terr)) {
      bt_app_set_status(app, terr->message);
      g_error_free(terr);
    }
    if (app->sessions_panel != NULL)
      bt_sessionspanel_refresh(app->sessions_panel, app->sessions);
  }

  update_header(app);
  gchar *msg = g_strdup_printf("Opened vault — %u notes (%.1f ms)", st.notes,
                               (double)st.elapsed_us / 1000.0);
  show_toast(app, msg, 2);
  g_free(msg);
  return TRUE;
}

static void ensure_view_stack_visible(BtApp *app)
{
  if (app->term_fullscreen) {
    app->term_fullscreen = FALSE;
    if (app->split_view != NULL)
      adw_overlay_split_view_set_show_sidebar(app->split_view, TRUE);
  }
  if (app->top_paned != NULL)
    gtk_widget_set_visible(app->top_paned, TRUE);
  if (app->view_stack != NULL)
    gtk_widget_set_visible(GTK_WIDGET(app->view_stack), TRUE);
}

static void set_graph_visible(BtApp *app, gboolean vis, gboolean persist)
{
  app->graph_visible = vis;
  if (vis)
    ensure_view_stack_visible(app);
  if (app->graphview != NULL)
    gtk_widget_set_visible(bt_graphview_widget(app->graphview), vis);
  if (app->graph_toggle != NULL) {
    app->syncing_graph_toggle = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->graph_toggle), vis);
    app->syncing_graph_toggle = FALSE;
  }
  if (vis && app->graphview != NULL) {
    if (!bt_graphview_has_graph(app->graphview))
      refresh_graph(app, !app->smoke_test);
    else
      gtk_widget_queue_draw(bt_graphview_widget(app->graphview));
  }
  if (persist && !app->smoke_test)
    save_graph_visible(vis);
}

static void on_graph_toggle_toggled(GtkToggleButton *btn, gpointer ud)
{
  BtApp *app = ud;
  if (app->syncing_graph_toggle)
    return;
  set_graph_visible(app, gtk_toggle_button_get_active(btn), TRUE);
}

static void on_view_stack_page_changed(GObject *obj, GParamSpec *pspec, gpointer ud)
{
  (void)pspec;
  BtApp *app = ud;
  const gchar *name = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(obj));
  if (name == NULL)
    return;
  if (!app->smoke_test)
    bt_app_save_page(name);
  if (strcmp(name, "graph") == 0 && app->graphview != NULL)
    gtk_widget_queue_draw(bt_graphview_widget(app->graphview));
}

gboolean bt_app_open_note(BtApp *app, const gchar *relpath, GError **error)
{
  g_return_val_if_fail(app != NULL && relpath != NULL, FALSE);
  if (app->vault == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "No vault is currently open");
    return FALSE;
  }
  ensure_view_stack_visible(app);
  if (app->view_stack != NULL)
    adw_view_stack_set_visible_child_name(app->view_stack, "editor");
  if (app->editor != NULL && g_strcmp0(bt_editorpane_relpath(app->editor), relpath) == 0)
    return TRUE;
  if (app->editor != NULL && bt_editorpane_is_dirty(app->editor))
    bt_editorpane_flush_save(app->editor);

  gchar *text = NULL;
  gsize len = 0;
  if (!vault_read(app->vault, relpath, &text, &len, error))
    return FALSE;

  if (!g_utf8_validate(text, (gssize)len, NULL)) {
    g_free(text);
    g_set_error(error, BT_ERROR, BT_ERROR_ENCODING, "Note '%s' is not valid UTF-8", relpath);
    return FALSE;
  }

  bt_editorpane_load(app->editor, relpath, text, len);
  bt_filetree_select_path(app->tree, relpath);
  update_header(app);
  g_free(text);
  return TRUE;
}

gboolean bt_app_open_or_create_wikilink(BtApp *app, const gchar *target, GError **error)
{
  g_return_val_if_fail(app != NULL && target != NULL, FALSE);
  if (app->vault == NULL)
    return FALSE;

  gchar *existing = bt_wikilink_find_note(vault_notes(app->vault), target);
  if (existing != NULL) {
    gboolean ok = bt_app_open_note(app, existing, error);
    g_free(existing);
    return ok;
  }

  gchar *rel = bt_wikilink_new_note_path(target);
  if (rel == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_PATH_REJECTED, "Invalid wiki-link target: '%s'", target);
    return FALSE;
  }
  gchar *stub = g_strdup_printf("# %s\n\n", target);
  gboolean ok = vault_atomic_write(app->vault, rel, stub, strlen(stub), NULL, error);
  g_free(stub);
  if (ok) {
    vault_rescan(app->vault, NULL, NULL);
    bt_filetree_populate(app->tree, app->vault);
    ok = bt_app_open_note(app, rel, error);
  }
  g_free(rel);
  return ok;
}

gboolean bt_app_save_active(BtApp *app, GError **error)
{
  (void)error;
  g_return_val_if_fail(app != NULL, FALSE);
  if (app->editor != NULL)
    bt_editorpane_flush_save(app->editor);
  return TRUE;
}

/* ----------------------------------------------------- agent launch M3 -- */

static void launch_agent(BtApp *app, BtAgentKind kind)
{
  if (app->vault == NULL)
    return;
  const BtAgentInfo *info = bt_agents_get(&app->agents, kind);
  if (info == NULL)
    return;

  if (app->editor != NULL && bt_editorpane_is_dirty(app->editor))
    bt_editorpane_flush_save(app->editor);

  gchar oid[BT_OID_HEX_LEN] = {0};
  GDateTime *now = g_date_time_new_now_local();
  gchar *ts = g_date_time_format_iso8601(now);
  g_date_time_unref(now);
  gchar *cmsg = g_strdup_printf("bt-launch %s %s", info->id, ts ? ts : "");
  g_free(ts);

  GError *err = NULL;
  if (!bt_checkpoint_take(vault_root(app->vault), cmsg, oid, &err)) {
    g_free(cmsg);
    if (!app->allow_launch_without_checkpoint) {
      gchar *detail = g_strdup_printf("Launch blocked (R10): checkpoint failed.\n%s",
                                      err ? err->message : "");
      bt_app_show_error(app, "Agent launch refused", detail);
      g_free(detail);
      g_clear_error(&err);
      return;
    }
    g_clear_error(&err);
  } else {
    g_free(cmsg);
    show_toast(app, "Checkpoint saved", 2);
  }

  const gchar *active_rel = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
  if (bt_termtabs_count(app->tabs) == 0) {
    if (!bt_termtabs_open(app->tabs, app->vault, NULL, active_rel, NULL, &err)) {
      bt_app_show_error(app, "Cannot open terminal", err->message);
      g_error_free(err);
      return;
    }
  }

  const gchar *cwd = bt_termtabs_active_cwd(app->tabs);
  gchar *resume = bt_agents_format_resume_cmd(info->id, NULL);
  const BtSession *sess = bt_sessions_add(app->sessions, info->id,
                                          cwd ? cwd : vault_root(app->vault), resume, active_rel,
                                          oid[0] ? oid : NULL, NULL);
  g_free(resume);
  if (sess != NULL)
    bt_termtabs_bind_active_session(app->tabs, sess->id);
  if (app->sessions_panel != NULL)
    bt_sessionspanel_refresh(app->sessions_panel, app->sessions);

  gchar *cmd = bt_agents_format_launch_cmd(kind, active_rel ? active_rel : "this vault");
  bt_termtabs_write_active(app->tabs, (const guint8 *)cmd, strlen(cmd));
  g_free(cmd);
  gtk_widget_set_visible(bt_termtabs_widget(app->tabs), TRUE);
  bt_termtabs_focus_active(app->tabs);
}

/* ----------------------------------------------------------- callbacks -- */

static void on_tree_select(const gchar *relpath, gpointer ud)
{
  BtApp *app = ud;
  GError *err = NULL;
  if (!bt_app_open_note(app, relpath, &err)) {
    bt_app_show_error(app, "Cannot open note", err->message);
    g_error_free(err);
  }
}

static void on_tree_open_term(const gchar *rel_dir, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return;
  const gchar *active_rel = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
  GError *err = NULL;
  if (!bt_termtabs_open(app->tabs, app->vault, rel_dir, active_rel, NULL, &err)) {
    bt_app_show_error(app, "Cannot open terminal here", err->message);
    g_error_free(err);
    return;
  }
  gtk_widget_set_visible(bt_termtabs_widget(app->tabs), TRUE);
}

static void on_editor_save(const gchar *relpath, const gchar *text, gsize length, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return;
  GError *err = NULL;
  if (vault_atomic_write(app->vault, relpath, text, length, NULL, &err)) {
    bt_editorpane_mark_saved(app->editor);
    if (app->index != NULL)
      bt_index_upsert_note(app->index, app->vault, relpath, NULL, NULL);
    refresh_graph(app, FALSE);
    update_header(app);
    gchar *msg = g_strdup_printf("Saved — %s", relpath);
    show_toast(app, msg, 2);
    g_free(msg);
  } else {
    if (app->toast_overlay != NULL && !app->smoke_test) {
      AdwToast *t = adw_toast_new("Save FAILED — kept in buffer");
      adw_toast_set_button_label(t, "Retry");
      adw_toast_set_action_name(t, "app.save-active");
      adw_toast_overlay_add_toast(app->toast_overlay, t);
    }
    g_clear_error(&err);
  }
}

static void on_editor_link(const gchar *target, gpointer ud)
{
  BtApp *app = ud;
  GError *err = NULL;
  if (!bt_app_open_or_create_wikilink(app, target, &err)) {
    bt_app_show_error(app, "Cannot open wiki-link", err->message);
    g_error_free(err);
  }
}

static void on_editor_dirty(gboolean dirty, gpointer ud)
{
  (void)dirty;
  update_header(ud);
}

static void on_quickopen_pick(const gchar *relpath, gpointer ud)
{
  on_tree_select(relpath, ud);
}

static void on_session_ended(const gchar *session_id, gpointer ud)
{
  BtApp *app = ud;
  if (app->sessions != NULL && session_id != NULL) {
    bt_sessions_mark_ended(app->sessions, session_id, NULL);
    if (app->sessions_panel != NULL)
      bt_sessionspanel_refresh(app->sessions_panel, app->sessions);
  }
}

static void on_session_resume(const gchar *session_id, gpointer ud)
{
  BtApp *app = ud;
  const BtSession *s = bt_sessions_find(app->sessions, session_id);
  if (s == NULL || app->vault == NULL)
    return;
  GError *err = NULL;
  if (!bt_termtabs_open(app->tabs, app->vault, NULL, s->note_id, s->id, &err)) {
    bt_app_show_error(app, "Cannot resume session", err->message);
    g_error_free(err);
    return;
  }
  if (s->resume_cmd != NULL && s->resume_cmd[0] != '\0')
    bt_termtabs_write_active(app->tabs, (const guint8 *)s->resume_cmd, strlen(s->resume_cmd));
  gtk_widget_set_visible(bt_termtabs_widget(app->tabs), TRUE);
}

static void on_session_rollback(const gchar *session_id, gpointer ud)
{
  BtApp *app = ud;
  const BtSession *s = bt_sessions_find(app->sessions, session_id);
  if (s == NULL || app->vault == NULL || s->checkpoint_oid[0] == '\0')
    return;
  GError *err = NULL;
  if (!bt_checkpoint_rollback(vault_root(app->vault), s->checkpoint_oid, &err)) {
    bt_app_show_error(app, "Rollback failed", err->message);
    g_error_free(err);
    return;
  }
  vault_rescan(app->vault, NULL, NULL);
  bt_filetree_populate(app->tree, app->vault);
  refresh_graph(app, FALSE);
  show_toast(app, "Rolled back vault to checkpoint", 3);
}

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
  BtApp *app = ud;
  GError *err = NULL;
  GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
  if (folder == NULL) {
    if (!g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
      bt_app_show_error(app, "Folder selection failed", err->message);
    g_clear_error(&err);
    return;
  }
  gchar *path = g_file_get_path(folder);
  g_object_unref(folder);
  if (path != NULL) {
    if (!bt_app_open_vault(app, path, &err)) {
      bt_app_show_error(app, "Cannot open vault", err->message);
      g_error_free(err);
    }
    g_free(path);
  }
}

static void prompt_open_vault(BtApp *app)
{
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Open BrainTerm Vault");
  gtk_file_dialog_select_folder(dlg, app->window, NULL, on_folder_chosen, app);
  g_object_unref(dlg);
}

static void on_missing_vault_response(GObject *src, GAsyncResult *res, gpointer ud)
{
  BtApp *app = ud;
  int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
  if (choice == 0)
    prompt_open_vault(app);
}

static void show_vault_not_found(BtApp *app, const gchar *missing_path)
{
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Vault not found");
  gchar *detail = g_strdup_printf("The previous vault folder could not be opened:\n%s",
                                  missing_path);
  gtk_alert_dialog_set_detail(dlg, detail);
  const gchar *const buttons[] = {"Open Vault...", "Cancel", NULL};
  gtk_alert_dialog_set_buttons(dlg, buttons);
  gtk_alert_dialog_set_default_button(dlg, 0);
  gtk_alert_dialog_set_cancel_button(dlg, 1);
  gtk_alert_dialog_choose(dlg, app->window, NULL, on_missing_vault_response, app);
  g_free(detail);
  g_object_unref(dlg);
}

static void on_watcher_event(BtWatchEventKind kind, const gchar *relpath, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return;
  if (kind == BT_WATCH_STORM_RESCAN) {
    vault_rescan(app->vault, NULL, NULL);
    if (app->tree) bt_filetree_populate(app->tree, app->vault);
    if (app->index) bt_watcher_audit_drift(app->vault, app->index, NULL);
    refresh_graph(app, FALSE);
    show_toast(app, "Rename storm detected — rescanned vault", 3);
    return;
  }
  if (kind == BT_WATCH_DELETED) {
    if (app->index) bt_index_delete_path(app->index, relpath, NULL);
    vault_rescan(app->vault, NULL, NULL);
    if (app->tree) bt_filetree_populate(app->tree, app->vault);
    if (app->editor && g_strcmp0(bt_editorpane_relpath(app->editor), relpath) == 0)
      bt_editorpane_show_conflict(app->editor, TRUE);
    refresh_graph(app, FALSE);
    return;
  }

  gboolean skipped = FALSE;
  if (app->index)
    bt_index_upsert_note(app->index, app->vault, relpath, &skipped, NULL);
  vault_rescan(app->vault, NULL, NULL);
  if (app->tree) bt_filetree_populate(app->tree, app->vault);
  refresh_graph(app, FALSE);

  if (app->editor && g_strcmp0(bt_editorpane_relpath(app->editor), relpath) == 0) {
    gchar *disk = NULL;
    gsize dlen = 0;
    if (vault_read(app->vault, relpath, &disk, &dlen, NULL)) {
      gchar *dh = bt_fnv1a_hex(disk, dlen);
      gchar *bh = bt_editorpane_buffer_hash(app->editor);
      BtConflictAction act = bt_conflict_decide(bt_editorpane_is_dirty(app->editor), FALSE, bh, dh);
      if (act == BT_CONFLICT_SILENT_RELOAD) {
        bt_editorpane_load(app->editor, relpath, disk, dlen);
        show_toast(app, "Reloaded externally modified note", 2);
      } else if (act == BT_CONFLICT_BANNER_DIRTY) {
        bt_editorpane_show_conflict(app->editor, FALSE);
      }
      g_free(dh);
      g_free(bh);
      g_free(disk);
    }
  }
}

static void on_editor_conflict_diff(const gchar *relpath, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return;
  gchar *cp_txt = NULL, *disk_txt = NULL;
  bt_checkpoint_read_file(vault_root(app->vault), "HEAD", relpath, &cp_txt, NULL, NULL);
  vault_read(app->vault, relpath, &disk_txt, NULL, NULL);
  gchar *msg = g_strdup_printf("Pre-agent checkpoint: %zu bytes\nOn-disk: %zu bytes\n\n"
                               "Use [Keep mine] or [Reload].",
                               cp_txt ? strlen(cp_txt) : 0, disk_txt ? strlen(disk_txt) : 0);
  bt_app_show_error(app, "Conflict Review (Checkpoint vs Disk)", msg);
  g_free(msg);
  g_free(cp_txt);
  g_free(disk_txt);
}

static void on_editor_conflict_reload(const gchar *relpath, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return;
  gchar *disk = NULL;
  gsize len = 0;
  if (vault_read(app->vault, relpath, &disk, &len, NULL)) {
    bt_editorpane_load(app->editor, relpath, disk, len);
    g_free(disk);
  } else {
    bt_editorpane_clear(app->editor);
  }
}

static void on_graph_open_note(const gchar *relpath, gboolean is_ghost, gpointer ud)
{
  BtApp *app = ud;
  GError *err = NULL;
  gboolean ok = is_ghost ? bt_app_open_or_create_wikilink(app, relpath, &err)
                         : bt_app_open_note(app, relpath, &err);
  if (!ok && err != NULL) {
    bt_app_show_error(app, "Cannot open graph node", err->message);
    g_error_free(err);
  } else if (is_ghost) {
    refresh_graph(app, TRUE);
  }
}

static void on_term_fullscreen(gpointer ud)
{
  BtApp *app = ud;
  app->term_fullscreen = !app->term_fullscreen;
  adw_overlay_split_view_set_show_sidebar(app->split_view, !app->term_fullscreen);
  if (app->top_paned != NULL)
    gtk_widget_set_visible(app->top_paned, !app->term_fullscreen);
  gtk_widget_set_visible(bt_termtabs_widget(app->tabs), TRUE);
  bt_termtabs_focus_active(app->tabs);
}

static void on_term_status(const gchar *msg, gpointer ud)
{
  show_toast(ud, msg, 2);
}

/* ------------------------------------------- Preferences & About (M-UI) -- */

static void on_pref_scheme_selected(GObject *obj, GParamSpec *pspec, gpointer ud)
{
  (void)pspec;
  BtApp *app = ud;
  guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
  BtUiScheme sc = (idx == 0) ? BT_SCHEME_SYSTEM : ((idx == 1) ? BT_SCHEME_DARK : BT_SCHEME_LIGHT);
  apply_scheme_runtime(app, sc, TRUE);
}

static void show_preferences_dialog(BtApp *app)
{
  GtkWidget *win = adw_window_new();
  gtk_window_set_title(GTK_WINDOW(win), "Preferences");
  gtk_window_set_transient_for(GTK_WINDOW(win), app->window);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(win), 420, 220);

  GtkWidget *tv = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), adw_header_bar_new());

  GtkWidget *grp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(grp), "Ink Appearance");
  gtk_widget_set_margin_top(grp, 16);
  gtk_widget_set_margin_bottom(grp, 16);
  gtk_widget_set_margin_start(grp, 16);
  gtk_widget_set_margin_end(grp, 16);

  GtkWidget *row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Color Scheme");
  const gchar *const opts[] = {"Follow System", "Dark (Ink)", "Light (Paper)", NULL};
  GtkStringList *sl = gtk_string_list_new(opts);
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(sl));
  g_object_unref(sl);
  guint cur = (app->scheme == BT_SCHEME_SYSTEM) ? 0 : ((app->scheme == BT_SCHEME_DARK) ? 1 : 2);
  adw_combo_row_set_selected(ADW_COMBO_ROW(row), cur);
  g_signal_connect(row, "notify::selected", G_CALLBACK(on_pref_scheme_selected), app);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(grp), row);

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), grp);
  adw_window_set_content(ADW_WINDOW(win), tv);
  gtk_window_present(GTK_WINDOW(win));
}

/* ---------------------------------------------------- GActions (§5.5) -- */

static void on_ask_run_action(const gchar *action_name, gpointer ud)
{
  BtApp *app = ud;
  const gchar *name = g_str_has_prefix(action_name, "app.") ? (action_name + 4) : action_name;
  g_action_group_activate_action(G_ACTION_GROUP(app->adw_app), name, NULL);
}

static void on_ask_send_agent(const gchar *cmd_line, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return;
  if (bt_termtabs_count(app->tabs) == 0)
    bt_termtabs_open(app->tabs, app->vault, NULL, NULL, NULL, NULL);
  bt_termtabs_write_active(app->tabs, (const guint8 *)cmd_line, strlen(cmd_line));
  gtk_widget_set_visible(bt_termtabs_widget(app->tabs), TRUE);
  bt_termtabs_focus_active(app->tabs);
}

static void act_open_vault(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; prompt_open_vault(ud); }
static void act_open_note(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; if (p) on_tree_select(g_variant_get_string(p, NULL), ud); }
static void act_save_active(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; bt_app_save_active(ud, NULL); }
static void act_term_spawn(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; on_tree_open_term(NULL, ud); }
static void act_launch_claude(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; launch_agent(ud, BT_AGENT_CLAUDE); }
static void act_launch_codex(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; launch_agent(ud, BT_AGENT_CODEX); }
static void act_launch_gemini(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; launch_agent(ud, BT_AGENT_GEMINI); }
static void act_preferences(GSimpleAction *a, GVariant *p, gpointer ud) { (void)a; (void)p; show_preferences_dialog(ud); }

static void act_new_note(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL) return;
  gchar *name = g_strdup_printf("note-%" G_GINT64_FORMAT, g_get_real_time() / 1000000);
  bt_app_open_or_create_wikilink(app, name, NULL);
  g_free(name);
}

static void act_quick_open(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault != NULL)
    bt_quickopen_show(app->window, app->vault, on_quickopen_pick, app);
}

static void act_rescan_vault(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL) return;
  BtScanStats st = {0};
  if (vault_rescan(app->vault, &st, NULL)) {
    bt_filetree_populate(app->tree, app->vault);
    update_header(app);
    gchar *msg = g_strdup_printf("Rescanned %u notes (%.1f ms)", st.notes, (double)st.elapsed_us / 1000.0);
    show_toast(app, msg, 2);
    g_free(msg);
  }
}

static void act_toggle_sidebar(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->split_view != NULL)
    adw_overlay_split_view_set_show_sidebar(app->split_view,
                                            !adw_overlay_split_view_get_show_sidebar(app->split_view));
}

static void act_toggle_terminal(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->tabs == NULL) return;
  GtkWidget *tw = bt_termtabs_widget(app->tabs);
  gboolean vis = !gtk_widget_get_visible(tw);
  if (!vis && app->term_fullscreen)
    on_term_fullscreen(app);
  gtk_widget_set_visible(tw, vis);
  if (vis) bt_termtabs_focus_active(app->tabs);
}

static void act_toggle_sessions(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->view_stack != NULL) {
    ensure_view_stack_visible(app);
    const gchar *cur = adw_view_stack_get_visible_child_name(app->view_stack);
    adw_view_stack_set_visible_child_name(app->view_stack,
                                          g_strcmp0(cur, "sessions") == 0 ? "editor" : "sessions");
  }
}

static void act_toggle_graph(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  set_graph_visible(app, !app->graph_visible, TRUE);
}

static void act_checkpoint(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL) return;
  gchar oid[BT_OID_HEX_LEN] = {0};
  GError *err = NULL;
  if (bt_checkpoint_take(vault_root(app->vault), "bt-manual checkpoint", oid, &err)) {
    gchar *msg = g_strdup_printf("Checkpoint %.8s", oid);
    show_toast(app, msg, 2);
    g_free(msg);
  } else {
    bt_app_show_error(app, "Checkpoint failed", err->message);
    g_error_free(err);
  }
}

static void act_rollback_latest(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL) return;
  GError *err = NULL;
  if (bt_checkpoint_rollback(vault_root(app->vault), "HEAD", &err)) {
    vault_rescan(app->vault, NULL, NULL);
    bt_filetree_populate(app->tree, app->vault);
    refresh_graph(app, FALSE);
    show_toast(app, "Rolled back to HEAD checkpoint", 3);
  } else {
    bt_app_show_error(app, "Rollback failed", err->message);
    g_error_free(err);
  }
}

static void act_ask(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  bt_askbar_show(app->window, app->index, on_tree_select, on_ask_run_action, on_ask_send_agent, app);
}

static void act_rebuild_index(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL || app->index == NULL) return;
  vault_rescan(app->vault, NULL, NULL);
  if (bt_index_rebuild(app->index, app->vault, NULL)) {
    refresh_graph(app, FALSE);
    gchar *msg = g_strdup_printf("Index rebuilt (%u notes)", bt_index_note_count(app->index));
    show_toast(app, msg, 2);
    g_free(msg);
  }
}

static void act_audit_index(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL || app->index == NULL) return;
  vault_rescan(app->vault, NULL, NULL);
  guint fixed = bt_watcher_audit_drift(app->vault, app->index, NULL);
  gchar *msg = g_strdup_printf("Audit complete — %u repaired", fixed);
  show_toast(app, msg, 2);
  g_free(msg);
}

static void act_organize_now(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL || app->index == NULL) return;
  BtOrgStats st;
  GError *err = NULL;
  if (bt_organizer_run_batch(app->vault, app->index, NULL, BT_ORG_NO_AI, NULL, NULL, NULL, &st, &err)) {
    refresh_graph(app, TRUE);
    gchar *msg = g_strdup_printf("Organized %u notes (%u tags proposed)", st.notes_processed, st.tags_proposed);
    show_toast(app, msg, 3);
    g_free(msg);
  } else {
    bt_app_show_error(app, "Organize failed", err ? err->message : "");
    g_clear_error(&err);
  }
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  bt_app_show_error(app, "BrainTerm 0.1.0",
                    "Local-first Markdown second-brain with embedded ConPTY/libvterm terminal and 2D Brain Graph.\n"
                    "Theme: Ink (monochrome).");
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->editor != NULL && bt_editorpane_is_dirty(app->editor))
    bt_editorpane_flush_save(app->editor);
  bt_termtabs_close_all(app->tabs);
  g_application_quit(G_APPLICATION(app->adw_app));
}

static const GActionEntry app_actions[] = {
  {"open-vault", act_open_vault, NULL, NULL, NULL, {0}},
  {"new-note", act_new_note, NULL, NULL, NULL, {0}},
  {"open-note", act_open_note, "s", NULL, NULL, {0}},
  {"save-active", act_save_active, NULL, NULL, NULL, {0}},
  {"quick-open", act_quick_open, NULL, NULL, NULL, {0}},
  {"rescan-vault", act_rescan_vault, NULL, NULL, NULL, {0}},
  {"toggle-sidebar", act_toggle_sidebar, NULL, NULL, NULL, {0}},
  {"term-spawn", act_term_spawn, NULL, NULL, NULL, {0}},
  {"toggle-terminal", act_toggle_terminal, NULL, NULL, NULL, {0}},
  {"toggle-sessions", act_toggle_sessions, NULL, NULL, NULL, {0}},
  {"launch-claude", act_launch_claude, NULL, NULL, NULL, {0}},
  {"launch-codex", act_launch_codex, NULL, NULL, NULL, {0}},
  {"launch-gemini", act_launch_gemini, NULL, NULL, NULL, {0}},
  {"checkpoint", act_checkpoint, NULL, NULL, NULL, {0}},
  {"rollback", act_rollback_latest, NULL, NULL, NULL, {0}},
  {"ask", act_ask, NULL, NULL, NULL, {0}},
  {"rebuild-index", act_rebuild_index, NULL, NULL, NULL, {0}},
  {"audit-index", act_audit_index, NULL, NULL, NULL, {0}},
  {"organize-now", act_organize_now, NULL, NULL, NULL, {0}},
  {"toggle-graph", act_toggle_graph, NULL, NULL, NULL, {0}},
  {"preferences", act_preferences, NULL, NULL, NULL, {0}},
  {"about", act_about, NULL, NULL, NULL, {0}},
  {"quit", act_quit, NULL, NULL, NULL, {0}},
};

static gboolean on_window_close(GtkWindow *win, gpointer ud)
{
  (void)win;
  BtApp *app = ud;
  if (app->editor != NULL && bt_editorpane_is_dirty(app->editor))
    bt_editorpane_flush_save(app->editor);
  bt_termtabs_close_all(app->tabs);
  return FALSE;
}

static void dump_bt_widget_tree(GtkWidget *w, int depth)
{
  (void)depth;
  if (w == NULL)
    return;
  const gchar *name = gtk_widget_get_name(w);
  if (name != NULL && g_str_has_prefix(name, "bt-")) {
    printf("BT_TREE name=%s type=%s vis=%d map=%d real=%d size=%dx%d\n",
           name, G_OBJECT_TYPE_NAME(w),
           gtk_widget_get_visible(w) ? 1 : 0,
           gtk_widget_get_mapped(w) ? 1 : 0,
           gtk_widget_get_realized(w) ? 1 : 0,
           gtk_widget_get_width(w), gtk_widget_get_height(w));
  }
  for (GtkWidget *c = gtk_widget_get_first_child(w); c != NULL;
       c = gtk_widget_get_next_sibling(c)) {
    dump_bt_widget_tree(c, depth + 1);
  }
}

static gboolean on_smoke_idle(gpointer ud)
{
  BtApp *app = ud;
  const gchar *cur_page = app->view_stack ? adw_view_stack_get_visible_child_name(app->view_stack) : NULL;
  const gchar *sc_id = app->editor ? bt_editorpane_scheme_id(app->editor) : "none";
  printf("BT_TREE_STATE page=%s scheme=%s\n", cur_page ? cur_page : "(null)", sc_id ? sc_id : "none");
  dump_bt_widget_tree(GTK_WIDGET(app->window), 0);

  GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
  GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, "bt-markdown");
  guint notes = app->vault ? vault_notes(app->vault)->len : 0;
  gboolean term_ok = bt_termtabs_any_running(app->tabs);
  guint tab_n = bt_termtabs_count(app->tabs);
  guint sess_n = app->sessions ? bt_sessions_list(app->sessions)->len : 0;

  gchar *tmp_repo = g_dir_make_tmp("bt-smoke-git-XXXXXX", NULL);
  gchar *f = g_build_filename(tmp_repo, "a.md", NULL);
  g_file_set_contents(f, "# smoke\n", -1, NULL);
  gchar oid[BT_OID_HEX_LEN] = {0};
  gboolean cp_ok = bt_checkpoint_take(tmp_repo, "bt-smoke", oid, NULL);
  g_free(f);
  g_free(tmp_repo);

  gboolean idx_ok = app->index != NULL && bt_index_note_count(app->index) == notes;
  GPtrArray *hits = app->index ? bt_index_search_fts(app->index, "BrainTerm", 10, NULL) : NULL;
  gboolean ask_ok = hits != NULL && hits->len > 0;
  g_clear_pointer(&hits, g_ptr_array_unref);

  BtGraph *g = (app->vault && app->index) ? bt_graph_build(app->vault, app->index, app->sessions) : NULL;
  guint gnodes = g ? g->note_node_count : 0;
  bt_graph_free(g);

  printf("BT_SMOKE ok notes=%u lang=%s term=%s ui=adw-ink scheme=%s tabs=%u checkpoint=%s sessions=%u index=%s ask=%s graph=nodes:%u\n",
         notes, lang ? gtk_source_language_get_id(lang) : "none", term_ok ? "alive" : "off",
         is_effective_dark(app) ? "dark" : "light", tab_n, cp_ok ? "ok" : "fail", sess_n,
         idx_ok ? "ok" : "fail", ask_ok ? "search" : "empty", gnodes);
  fflush(stdout);
  bt_termtabs_close_all(app->tabs);
  g_application_quit(G_APPLICATION(app->adw_app));
  return G_SOURCE_REMOVE;
}

static GMenu *build_primary_menu(void)
{
  GMenu *m = g_menu_new();
  GMenu *s1 = g_menu_new();
  g_menu_append(s1, "Open Vault…", "app.open-vault");
  g_menu_append(s1, "New Note", "app.new-note");
  g_menu_append_section(m, NULL, G_MENU_MODEL(s1));

  GMenu *s2 = g_menu_new();
  g_menu_append(s2, "Rebuild Index", "app.rebuild-index");
  g_menu_append(s2, "Audit Index", "app.audit-index");
  g_menu_append(s2, "Organize Vault", "app.organize-now");
  g_menu_append(s2, "Checkpoint", "app.checkpoint");
  g_menu_append(s2, "Rollback…", "app.rollback");
  g_menu_append_section(m, NULL, G_MENU_MODEL(s2));

  GMenu *s3 = g_menu_new();
  g_menu_append(s3, "Preferences", "app.preferences");
  g_menu_append(s3, "About BrainTerm", "app.about");
  g_menu_append_section(m, NULL, G_MENU_MODEL(s3));

  g_object_unref(s1);
  g_object_unref(s2);
  g_object_unref(s3);
  return m;
}

static GMenu *build_agent_menu(void)
{
  GMenu *m = g_menu_new();
  GMenu *s1 = g_menu_new();
  g_menu_append(s1, "Codex here", "app.launch-codex");
  g_menu_append(s1, "Gemini here", "app.launch-gemini");
  g_menu_append_section(m, NULL, G_MENU_MODEL(s1));

  GMenu *s2 = g_menu_new();
  g_menu_append(s2, "Rollback to checkpoint", "app.rollback");
  g_menu_append_section(m, NULL, G_MENU_MODEL(s2));

  g_object_unref(s1);
  g_object_unref(s2);
  return m;
}

static void on_activate(GtkApplication *gtk_app, gpointer ud)
{
  BtApp *app = ud;
  if (app->window != NULL) {
    gtk_window_present(app->window);
    return;
  }

  install_theme(app);

  app->window = GTK_WINDOW(adw_application_window_new(gtk_app));
  gtk_widget_set_name(GTK_WIDGET(app->window), "bt-window");
  gtk_window_set_default_size(app->window, 1280, 800);
  g_signal_connect(app->window, "close-request", G_CALLBACK(on_window_close), app);

  /* Single flat AdwHeaderBar (§B.3–B.4): left = sidebar + vault title; center = empty; right = switcher + Agent + menu */
  GtkWidget *header = adw_header_bar_new();
  gtk_widget_set_name(header, "bt-header");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  GtkWidget *sb_btn = gtk_button_new_from_icon_name("sidebar-show-symbolic");
  gtk_widget_set_tooltip_text(sb_btn, "Toggle Sidebar (F9)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(sb_btn), "app.toggle-sidebar");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), sb_btn);

  app->win_title = ADW_WINDOW_TITLE(adw_window_title_new("BrainTerm", "No vault open"));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(app->win_title));

  /* Right side: MenuButton, Agent SplitButton, ViewSwitcher */
  GtkWidget *menu_btn = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
  GMenu *pmenu = build_primary_menu();
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(pmenu));
  g_object_unref(pmenu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_btn);

  app->agent_split = ADW_SPLIT_BUTTON(adw_split_button_new());
  adw_split_button_set_label(app->agent_split, "Agent");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(app->agent_split), "app.launch-claude");
  GMenu *amenu = build_agent_menu();
  adw_split_button_set_menu_model(app->agent_split, G_MENU_MODEL(amenu));
  g_object_unref(amenu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(app->agent_split));

  /* Content views inside AdwViewStack: Editor / Graph / Sessions */
  app->tree = bt_filetree_new(on_tree_select, on_tree_open_term, app);
  app->editor = bt_editorpane_new(on_editor_save, on_editor_link, on_editor_dirty, app);
  bt_editorpane_set_conflict_cbs(app->editor, on_editor_conflict_diff, on_editor_conflict_reload);
  app->graphview = bt_graphview_new(on_graph_open_note, on_tree_open_term, app);
  app->tabs = bt_termtabs_new(BT_TERMTABS_MAX_DEFAULT, on_term_fullscreen, on_term_status,
                              on_session_ended, app);
  app->sessions_panel = bt_sessionspanel_new(on_session_resume, on_session_rollback, app);

  app->view_stack = ADW_VIEW_STACK(adw_view_stack_new());
  gtk_widget_set_name(GTK_WIDGET(app->view_stack), "bt-view-stack");
  gtk_widget_set_hexpand(GTK_WIDGET(app->view_stack), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(app->view_stack), TRUE);
  adw_view_stack_add_titled_with_icon(app->view_stack, bt_editorpane_widget(app->editor),
                                      "editor", "Editor", "document-edit-symbolic");
  adw_view_stack_add_titled_with_icon(app->view_stack, bt_sessionspanel_widget(app->sessions_panel),
                                      "sessions", "Sessions", "utilities-terminal-symbolic");

  gchar *saved_page = app->smoke_test ? g_strdup("editor") : bt_app_load_saved_page();
  const gchar *stack_page = (g_strcmp0(saved_page, "sessions") == 0) ? "sessions" : "editor";
  adw_view_stack_set_visible_child_name(app->view_stack, stack_page);

  /* Dual-pane toggle button for the side-by-side Brain Graph on the right. */
  app->graph_visible = app->smoke_test ? TRUE : load_saved_graph_visible();
  if (g_strcmp0(saved_page, "graph") == 0)
    app->graph_visible = TRUE;

  app->graph_toggle = gtk_toggle_button_new();
  GtkWidget *gbtn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(gbtn_box), gtk_image_new_from_icon_name("network-workgroup-symbolic"));
  gtk_box_append(GTK_BOX(gbtn_box), gtk_label_new("Graph"));
  gtk_button_set_child(GTK_BUTTON(app->graph_toggle), gbtn_box);
  gtk_widget_add_css_class(app->graph_toggle, "bt-hairline");
  gtk_widget_set_tooltip_text(app->graph_toggle, "Toggle Side-by-Side Graph (Ctrl+G)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->graph_toggle), app->graph_visible);
  g_signal_connect(app->graph_toggle, "toggled", G_CALLBACK(on_graph_toggle_toggled), app);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), app->graph_toggle);

  GtkWidget *switcher = adw_view_switcher_new();
  adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), app->view_stack);
  adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), switcher);

  /* Top dual-pane split: Editor/Sessions (left) | Brain Graph (right) */
  app->top_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_name(app->top_paned, "bt-top-paned");
  gtk_widget_set_hexpand(app->top_paned, TRUE);
  gtk_widget_set_vexpand(app->top_paned, TRUE);
  gtk_paned_set_start_child(GTK_PANED(app->top_paned), GTK_WIDGET(app->view_stack));
  gtk_paned_set_end_child(GTK_PANED(app->top_paned), bt_graphview_widget(app->graphview));
  gtk_paned_set_position(GTK_PANED(app->top_paned), 620);
  gtk_paned_set_resize_start_child(GTK_PANED(app->top_paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(app->top_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(app->top_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(app->top_paned), FALSE);
  gtk_widget_set_visible(bt_graphview_widget(app->graphview), app->graph_visible);

  /* Right workspace: Dual-pane Editor|Graph (top) / Multi-tab Terminal (bottom) */
  app->right_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name(app->right_paned, "bt-right-paned");
  gtk_widget_set_hexpand(app->right_paned, TRUE);
  gtk_widget_set_vexpand(app->right_paned, TRUE);
  gtk_paned_set_start_child(GTK_PANED(app->right_paned), app->top_paned);
  gtk_paned_set_end_child(GTK_PANED(app->right_paned), bt_termtabs_widget(app->tabs));
  gtk_paned_set_position(GTK_PANED(app->right_paned), 460);
  gtk_paned_set_resize_start_child(GTK_PANED(app->right_paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(app->right_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(app->right_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(app->right_paned), FALSE);

  /* Collapsible notes sidebar (§B.5) */
  GtkWidget *sb_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(sb_box, "bt-sidebar");
  gtk_widget_add_css_class(sb_box, "bt-sidebar");
  app->sidebar_vault_lbl = gtk_label_new("● vault");
  gtk_label_set_xalign(GTK_LABEL(app->sidebar_vault_lbl), 0.0f);
  gtk_widget_add_css_class(app->sidebar_vault_lbl, "bt-sidebar-header");
  gtk_box_append(GTK_BOX(sb_box), app->sidebar_vault_lbl);
  gtk_box_append(GTK_BOX(sb_box), bt_filetree_widget(app->tree));

  app->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  gtk_widget_set_name(GTK_WIDGET(app->split_view), "bt-split-view");
  adw_overlay_split_view_set_sidebar(app->split_view, sb_box);
  adw_overlay_split_view_set_content(app->split_view, app->right_paned);
  adw_overlay_split_view_set_max_sidebar_width(app->split_view, 260.0);

  app->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_toast_overlay_set_child(app->toast_overlay, GTK_WIDGET(app->split_view));

  GtkWidget *tview = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tview), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tview), GTK_WIDGET(app->toast_overlay));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(app->window), tview);

  /* Apply persisted scheme (dark-first default) before presenting. */
  apply_scheme_runtime(app, app->smoke_test ? BT_SCHEME_DARK : load_saved_scheme(), FALSE);

  gtk_window_present(app->window);

  gchar *initial = app->cli_vault ? g_strdup(app->cli_vault) : bt_app_load_last_vault();
  if (initial != NULL && initial[0] != '\0') {
    GError *err = NULL;
    if (!bt_app_open_vault(app, initial, &err)) {
      if (!app->smoke_test && g_error_matches(err, BT_ERROR, BT_ERROR_NOT_FOUND))
        show_vault_not_found(app, initial);
      else
        bt_app_show_error(app, "Cannot open vault", err->message);
      g_error_free(err);
    } else if (vault_notes(app->vault)->len > 0) {
      const gchar *first = g_ptr_array_index(vault_notes(app->vault), 0);
      bt_app_open_note(app, first, NULL);
    }
  }
  if (!app->smoke_test && g_strcmp0(stack_page, "editor") != 0)
    adw_view_stack_set_visible_child_name(app->view_stack, stack_page);
  g_signal_connect(app->view_stack, "notify::visible-child-name",
                   G_CALLBACK(on_view_stack_page_changed), app);
  g_free(saved_page);
  g_free(initial);

  if (app->smoke_test)
    g_timeout_add(250, on_smoke_idle, app);
}

static gint on_handle_local_options(GApplication *gapp, GVariantDict *opts, gpointer ud)
{
  (void)gapp;
  BtApp *app = ud;
  gchar *vault = NULL;
  if (g_variant_dict_lookup(opts, "vault", "s", &vault)) {
    g_free(app->cli_vault);
    app->cli_vault = vault;
  }
  if (g_variant_dict_contains(opts, "smoke-test"))
    app->smoke_test = TRUE;
  return -1;
}

static void on_startup(GApplication *gapp, gpointer ud)
{
  BtApp *app = ud;
  g_action_map_add_action_entries(G_ACTION_MAP(gapp), app_actions, G_N_ELEMENTS(app_actions), app);

  static const gchar *const save_accels[] = {"<Primary>s", NULL};
  static const gchar *const quick_accels[] = {"<Primary>p", NULL};
  static const gchar *const ask_accels[] = {"<Primary>e", NULL};
  static const gchar *const open_accels[] = {"<Primary>o", NULL};
  static const gchar *const rescan_accels[] = {"F5", NULL};
  static const gchar *const sidebar_accels[] = {"F9", NULL};
  static const gchar *const graph_accels[] = {"<Primary>g", NULL};
  static const gchar *const term_accels[] = {"<Primary>grave", NULL};
  static const gchar *const quit_accels[] = {"<Primary>q", NULL};
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.save-active", save_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.quick-open", quick_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.ask", ask_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.open-vault", open_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.rescan-vault", rescan_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-sidebar", sidebar_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-graph", graph_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-terminal", term_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.quit", quit_accels);
}

BtApp *bt_app_new(void)
{
  bt_checkpoint_init();
  BtApp *app = g_new0(BtApp, 1);
  bt_agents_detect(&app->agents);
  app->scheme = BT_SCHEME_DARK;
  app->adw_app = adw_application_new("local.brainterm.App", G_APPLICATION_NON_UNIQUE);

  static const GOptionEntry opts[] = {
    {"vault", 'v', 0, G_OPTION_ARG_STRING, NULL, "Open vault directory", "DIR"},
    {"smoke-test", 0, 0, G_OPTION_ARG_NONE, NULL, "Open UI, verify startup, and exit", NULL},
    {NULL, 0, 0, 0, NULL, NULL, NULL},
  };
  g_application_add_main_option_entries(G_APPLICATION(app->adw_app), opts);

  g_signal_connect(app->adw_app, "handle-local-options", G_CALLBACK(on_handle_local_options), app);
  g_signal_connect(app->adw_app, "startup", G_CALLBACK(on_startup), app);
  g_signal_connect(app->adw_app, "activate", G_CALLBACK(on_activate), app);
  return app;
}

void bt_app_free(BtApp *app)
{
  if (app == NULL)
    return;
  bt_termtabs_free(app->tabs);
  bt_sessionspanel_free(app->sessions_panel);
  bt_sessions_free(app->sessions);
  bt_watcher_free(app->watcher);
  bt_index_close(app->index);
  bt_graphview_free(app->graphview);
  bt_editorpane_free(app->editor);
  bt_filetree_free(app->tree);
  vault_free(app->vault);
  bt_agents_clear(&app->agents);
  bt_themeloader_free(app->themeloader);
  g_free(app->cli_vault);
  g_clear_object(&app->adw_app);
  g_free(app);
  bt_checkpoint_shutdown();
}

int bt_app_run(BtApp *app, int argc, char **argv)
{
  g_return_val_if_fail(app != NULL, 1);
  return g_application_run(G_APPLICATION(app->adw_app), argc, argv);
}

BtVault *bt_app_vault(const BtApp *app) { return app ? app->vault : NULL; }
GtkWindow *bt_app_window(const BtApp *app) { return app ? app->window : NULL; }
