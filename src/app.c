/* app.c — BtApp with libadwaita Ink reskin (ADR-009, M-UI).
 * Presentation-layer reskin only; all M1–M8 actions & core contracts intact. */
#include "app.h"
#include "agents.h"
#include "bt_build_stamp.h"
#include "checkpoint.h"
#include "createnote.h"
#include "frontmatter.h"
#include "graph.h"
#include "graphview.h"
#include "index.h"
#include "linkresolve.h"
#include "navhistory.h"
#include "noteutil.h"
#include "organizer.h"
#include "sessions.h"
#include "theme.h"
#include "ui/askbar.h"
#include "ui/chooserpopover.h"
#include "ui/createpopover.h"
#include "ui/editorpane.h"
#include "ui/filetree.h"
#include "ui/quickopen.h"
#include "ui/sessionspanel.h"
#include "ui/termtabs.h"
#include "ui/themeload.h"
#include "watcher.h"

#include <adwaita.h>
#include <cjson/cJSON.h>
#include <gtksourceview/gtksource.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
  GtkWidget *graph_card;
  GtkWidget *term_card;
  GtkWidget *seg_editor;
  GtkWidget *seg_sessions;
  GtkWidget *seg_graph;
  GtkWidget *status_vault_lbl;
  GtkWidget *status_notes_lbl;
  GtkWidget *status_git_box;
  GtkWidget *status_git_lbl;
  GtkWidget *status_graph_notice;
  GtkWidget *status_notice_lbl;
  gchar *last_statusbar_notice;
  BtThemeLoader *themeloader;

  BtVault *vault;
  BtIndex *index;
  BtIndexSnapshot *link_snap;
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
  gboolean syncing_nav;
  gboolean term_fullscreen;
  gboolean allow_launch_without_checkpoint; /* R10: FALSE by default */
  gchar last_checkpoint_oid[BT_OID_HEX_LEN];

  gchar *cli_vault;
  gboolean smoke_test;
  gboolean graph_debug;
  gchar *capture_dir;
  gchar *capture_state;
  gint capture_w;
  gint capture_h;
  gsize capture_idx;
  guint capture_settle_left;
  gboolean capture_applied;
  gboolean capture_failed;
  gboolean fail_next_save_for_test;
  gboolean confirm_external_urls;
  gboolean confirm_external_urls_corrupt_pending_log;
  gboolean session_external_confirmed;
  BtLinkLauncherFn test_launcher;
  gpointer test_launcher_ud;
  GtkPopover *active_confirm_popover;
  gchar *active_confirm_detail;
  BtLinkResolution *pending_confirm_res;
  gboolean pending_confirm_dont_ask;

  BtNavHistory *nav;
  gboolean nav_replaying;
  gboolean links_open_on_click;

  BtCreatePopover *create_popover;
  BtChooserPopover *chooser_popover;
  gchar *pending_chooser_anchor;
  gchar *last_copied_link;
  guint status_notice_timer_id;
  guint deferred_graph_idle_id;
  gboolean create_busy;
  struct BtAppRef *self_ref;

  gchar *verify_l2_dir;
  guint verify_l2_phase;
  guint verify_l2_settle;
  guint verify_l2_frames;
  gint64 verify_l2_prev_fc_us;
  GArray *verify_l2_frame_ms;
  GArray *verify_l2_work_ms;
  gboolean verify_l2_nav_back_ok;

  gchar *bench_first_open_out;
  gchar *bench_first_open_note;
  guint bench_fo_phase;
  guint bench_fo_settle;
  guint bench_fo_frames;
  gint64 bench_fo_prev_fc_us;
  gint64 bench_fo_open_start_us;
  gdouble bench_fo_cold_open_ms;
  gdouble bench_fo_cold_first_frame_ms;
  gdouble bench_fo_cold_wall_frame_ms;
  GArray *bench_fo_cold_frames;
  gdouble bench_fo_warm_open_ms;
  gdouble bench_fo_warm_first_frame_ms;
  gdouble bench_fo_warm_wall_frame_ms;
  GArray *bench_fo_warm_frames;
};

static inline gboolean app_is_ro_bench(const BtApp *app)
{
  return app != NULL && (app->verify_l2_dir != NULL || app->bench_first_open_out != NULL);
}

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

static gboolean load_saved_graph_bool(const gchar *key, gboolean def_val)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  gboolean val = def_val;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    gchar *v = g_key_file_get_string(kf, "graph", key, NULL);
    if (v != NULL) {
      val = (g_strcmp0(v, "1") == 0 || g_ascii_strcasecmp(v, "true") == 0);
      g_free(v);
    }
  }
  g_key_file_unref(kf);
  g_free(path);
  return val;
}

static void on_graph_prefs_changed(gboolean show_tags, gboolean show_ghosts, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->smoke_test)
    return;
  save_state_str("graph", "show_tags", show_tags ? "1" : "0", NULL);
  save_state_str("graph", "show_ghosts", show_ghosts ? "1" : "0", NULL);
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

gboolean bt_app_load_links_open_on_click(void)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  gboolean val = TRUE; /* ADR-016 default: single-click opens links */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    gchar *v = g_key_file_get_string(kf, "editor", "links_open_on_click", NULL);
    if (v != NULL) {
      val = (g_strcmp0(v, "0") != 0 && g_ascii_strcasecmp(v, "false") != 0);
      g_free(v);
    }
  }
  g_key_file_unref(kf);
  g_free(path);
  return val;
}

gboolean bt_app_save_links_open_on_click(gboolean enabled, GError **error)
{
  return save_state_str("editor", "links_open_on_click", enabled ? "true" : "false", error);
}

void bt_app_set_links_open_on_click(BtApp *app, gboolean enabled, gboolean persist)
{
  if (app == NULL)
    return;
  app->links_open_on_click = enabled ? TRUE : FALSE;
  if (app->editor != NULL)
    bt_editorpane_set_links_open_on_click(app->editor, app->links_open_on_click);
  if (app->adw_app != NULL) {
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(app->adw_app), "links-single-click");
    if (act != NULL && G_IS_SIMPLE_ACTION(act))
      g_simple_action_set_state(G_SIMPLE_ACTION(act),
                                g_variant_new_boolean(app->links_open_on_click));
  }
  if (persist)
    bt_app_save_links_open_on_click(app->links_open_on_click, NULL);
}

gboolean bt_app_get_links_open_on_click(const BtApp *app)
{
  return app ? app->links_open_on_click : TRUE;
}

static gboolean parse_confirm_external_urls_raw(const gchar *raw, gboolean *out_corrupt)
{
  if (out_corrupt != NULL)
    *out_corrupt = FALSE;
  if (raw == NULL)
    return TRUE;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(raw));
  if (g_strcmp0(trimmed, "1") == 0 || g_ascii_strcasecmp(trimmed, "true") == 0)
    return TRUE;
  if (g_strcmp0(trimmed, "0") == 0 || g_ascii_strcasecmp(trimmed, "false") == 0)
    return FALSE;
  if (out_corrupt != NULL)
    *out_corrupt = TRUE;
  return TRUE;
}

static gboolean load_confirm_external_urls_internal(const BtVault *vault, gboolean *out_corrupt)
{
  gchar *path = bt_app_state_file_path();
  GKeyFile *kf = g_key_file_new();
  gboolean val = TRUE; /* L3-4 default: confirm external URLs on first session open */
  gboolean corrupt = FALSE;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    gchar *v = g_key_file_get_string(kf, "editor", "confirm_external_urls", NULL);
    if (v != NULL) {
      val = parse_confirm_external_urls_raw(v, &corrupt);
      g_free(v);
    }
  }
  g_key_file_unref(kf);
  g_free(path);
  if (corrupt && vault != NULL)
    bt_vault_log(vault, "WARNING",
                 "state.ini invalid [editor] confirm_external_urls; falling back to true");
  if (out_corrupt != NULL)
    *out_corrupt = corrupt;
  return val;
}

gboolean bt_app_load_confirm_external_urls(const BtVault *vault)
{
  return load_confirm_external_urls_internal(vault, NULL);
}

gboolean bt_app_save_confirm_external_urls(gboolean enabled, GError **error)
{
  return save_state_str("editor", "confirm_external_urls", enabled ? "true" : "false", error);
}

void bt_app_set_confirm_external_urls(BtApp *app, gboolean enabled, gboolean persist)
{
  if (app == NULL)
    return;
  app->confirm_external_urls = enabled ? TRUE : FALSE;
  if (app->adw_app != NULL) {
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(app->adw_app), "confirm-external-urls");
    if (act != NULL && G_IS_SIMPLE_ACTION(act))
      g_simple_action_set_state(G_SIMPLE_ACTION(act),
                                g_variant_new_boolean(app->confirm_external_urls));
  }
  if (persist)
    bt_app_save_confirm_external_urls(app->confirm_external_urls, NULL);
}

gboolean bt_app_get_confirm_external_urls(const BtApp *app)
{
  return app ? app->confirm_external_urls : TRUE;
}

gpointer bt_app_application(const BtApp *app)
{
  return app ? app->adw_app : NULL;
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

static void update_statusbar(BtApp *app)
{
  if (app->status_vault_lbl == NULL)
    return;
  if (app->vault != NULL) {
    g_autofree gchar *base = g_path_get_basename(vault_root(app->vault));
    gtk_label_set_text(GTK_LABEL(app->status_vault_lbl), base);
    g_autofree gchar *nc = g_strdup_printf("%u notes", vault_notes(app->vault)->len);
    gtk_label_set_text(GTK_LABEL(app->status_notes_lbl), nc);
  } else {
    gtk_label_set_text(GTK_LABEL(app->status_vault_lbl), "No vault");
    gtk_label_set_text(GTK_LABEL(app->status_notes_lbl), "0 notes");
  }
  if (app->status_git_lbl != NULL && app->status_git_box != NULL) {
    gtk_widget_remove_css_class(app->status_git_box, "bt-status-ok");
    if (app->last_checkpoint_oid[0] != '\0') {
      g_autofree gchar *gs = g_strdup_printf("checkpoint %.8s", app->last_checkpoint_oid);
      gtk_label_set_text(GTK_LABEL(app->status_git_lbl), gs);
      gtk_widget_add_css_class(app->status_git_box, "bt-status-ok");
    } else if (app->vault != NULL) {
      gtk_label_set_text(GTK_LABEL(app->status_git_lbl), "checkpoint ready");
    } else {
      gtk_label_set_text(GTK_LABEL(app->status_git_lbl), "no git checkpoint");
    }
  }
}

static void update_header(BtApp *app)
{
  if (app->win_title == NULL)
    return;
  const gchar *note = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
  gboolean dirty = app->editor ? bt_editorpane_is_dirty(app->editor) : FALSE;

  if (app->vault != NULL) {
    gchar *base = g_path_get_basename(vault_root(app->vault));
    adw_window_title_set_title(app->win_title, base);
    gchar *sub = note ? g_strdup_printf("%s%s · %u notes", dirty ? "● " : "", note,
                                        vault_notes(app->vault)->len)
                      : g_strdup_printf("%u notes", vault_notes(app->vault)->len);
    adw_window_title_set_subtitle(app->win_title, sub);
    if (app->sidebar_vault_lbl != NULL)
      gtk_label_set_text(GTK_LABEL(app->sidebar_vault_lbl), base);
    g_free(sub);
    g_free(base);
  } else {
    adw_window_title_set_title(app->win_title, "BrainTerm");
    adw_window_title_set_subtitle(app->win_title, "No vault open");
    if (app->sidebar_vault_lbl != NULL)
      gtk_label_set_text(GTK_LABEL(app->sidebar_vault_lbl), "Vault");
  }
  update_statusbar(app);
}

static void show_toast(BtApp *app, const gchar *msg, guint timeout_s)
{
  if (app == NULL || app->toast_overlay == NULL || app->smoke_test ||
      app_is_ro_bench(app) || app->capture_dir != NULL || msg == NULL)
    return;
  AdwToast *t = adw_toast_new(msg);
  adw_toast_set_timeout(t, timeout_s);
  adw_toast_overlay_add_toast(app->toast_overlay, t);
}

void bt_app_set_status(BtApp *app, const gchar *message)
{
  show_toast(app, message, 2);
}

void bt_app_set_statusbar_notice(BtApp *app, const gchar *message)
{
  if (app == NULL)
    return;
  if (app->status_notice_timer_id != 0) {
    g_source_remove(app->status_notice_timer_id);
    app->status_notice_timer_id = 0;
  }
  g_free(app->last_statusbar_notice);
  app->last_statusbar_notice = g_strdup(message ? message : "");
  if (app->status_notice_lbl != NULL) {
    if (message != NULL && message[0] != '\0') {
      gtk_label_set_text(GTK_LABEL(app->status_notice_lbl), message);
      gtk_widget_set_visible(app->status_notice_lbl, TRUE);
    } else {
      gtk_label_set_text(GTK_LABEL(app->status_notice_lbl), "");
      gtk_widget_set_visible(app->status_notice_lbl, FALSE);
    }
  }
}

static gboolean on_status_notice_clear_cb(gpointer ud)
{
  BtApp *app = ud;
  app->status_notice_timer_id = 0;
  bt_app_set_statusbar_notice(app, "");
  return G_SOURCE_REMOVE;
}

static void set_statusbar_notice_timed(BtApp *app, const gchar *message, guint timeout_s)
{
  bt_app_set_statusbar_notice(app, message);
  if (app != NULL && message != NULL && message[0] != '\0' && timeout_s > 0u)
    app->status_notice_timer_id = g_timeout_add_seconds(timeout_s, on_status_notice_clear_cb, app);
}

const gchar *bt_app_last_statusbar_notice(const BtApp *app)
{
  return app ? app->last_statusbar_notice : NULL;
}

void bt_app_set_fail_next_save_for_test(BtApp *app, gboolean fail)
{
  if (app != NULL)
    app->fail_next_save_for_test = fail;
}

const gchar *bt_app_active_note(const BtApp *app)
{
  return (app && app->editor) ? bt_editorpane_relpath(app->editor) : NULL;
}

gpointer bt_app_editor(const BtApp *app)
{
  return app ? app->editor : NULL;
}

static void reveal_in_file_manager(BtApp *app, const gchar *relpath, gboolean is_dir);
static void on_startup(GApplication *gapp, gpointer ud);
static void on_activate(GtkApplication *gtk_app, gpointer ud);

static void rebuild_link_snapshot(BtApp *app)
{
  if (app == NULL)
    return;
  if (app->editor != NULL)
    bt_editorpane_set_index_snapshot(app->editor, NULL, NULL);
  g_clear_pointer(&app->link_snap, bt_index_snapshot_free);
  if (app->vault != NULL)
    app->link_snap = bt_index_snapshot_from_vault(app->vault);
  if (app->editor != NULL)
    bt_editorpane_set_index_snapshot(app->editor, app->link_snap, app->vault);
}

static void clear_active_confirm(BtApp *app)
{
  if (app == NULL)
    return;
  if (app->active_confirm_popover != NULL) {
    GtkPopover *pop = app->active_confirm_popover;
    app->active_confirm_popover = NULL;
    gtk_popover_popdown(pop);
    if (gtk_widget_get_parent(GTK_WIDGET(pop)) != NULL)
      gtk_widget_unparent(GTK_WIDGET(pop));
  }
  g_clear_pointer(&app->active_confirm_detail, g_free);
  g_clear_pointer(&app->pending_confirm_res, bt_link_resolution_free);
}

typedef struct BtAppRef {
  gint ref_count;
  BtApp *app;
} BtAppRef;

static BtAppRef *bt_app_ref_new(BtApp *app)
{
  BtAppRef *r = g_new0(BtAppRef, 1);
  r->ref_count = 1;
  r->app = app;
  return r;
}

static BtAppRef *bt_app_ref_acquire(BtAppRef *r)
{
  if (r != NULL)
    g_atomic_int_inc(&r->ref_count);
  return r;
}

static void bt_app_ref_release(BtAppRef *r)
{
  if (r != NULL && g_atomic_int_dec_and_test(&r->ref_count))
    g_free(r);
}

static void refresh_graph(BtApp *app, gboolean animate);

static gboolean on_deferred_graph_idle(gpointer ud)
{
  BtApp *app = ud;
  app->deferred_graph_idle_id = 0;
  refresh_graph(app, FALSE);
  return G_SOURCE_REMOVE;
}

static void schedule_graph_refresh_idle(BtApp *app)
{
  if (app == NULL || app->graphview == NULL || app->deferred_graph_idle_id != 0)
    return;
  app->deferred_graph_idle_id = g_idle_add(on_deferred_graph_idle, app);
}

static BtCreatePlan *clone_create_plan(const BtCreatePlan *src)
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

typedef struct {
  BtAppRef *app_ref;
  const BtVault *vault;
  BtCreatePlan *plan;
  gchar *source_note;
  gchar *anchor;
  BtCreateCommitResult commit_res;
  gchar *committed_relpath;
  gchar *committed_id;
  gchar *message;
} CreateTaskCtx;

static void create_task_ctx_free(gpointer p)
{
  CreateTaskCtx *ctx = p;
  if (ctx == NULL)
    return;
  bt_app_ref_release(ctx->app_ref);
  bt_createnote_plan_free(ctx->plan);
  g_free(ctx->source_note);
  g_free(ctx->anchor);
  g_free(ctx->committed_relpath);
  g_free(ctx->committed_id);
  g_free(ctx->message);
  g_free(ctx);
}

static void create_note_worker(GTask *task, gpointer source_obj, gpointer task_data,
                               GCancellable *cancellable)
{
  (void)source_obj; (void)cancellable;
  CreateTaskCtx *ctx = task_data;
  GError *err = NULL;
  ctx->commit_res = bt_createnote_commit(ctx->vault, ctx->plan, &ctx->committed_relpath,
                                         &ctx->committed_id, &ctx->message, &err);
  if (err != NULL) {
    if (ctx->message == NULL)
      ctx->message = g_strdup_printf("Create failed: %s", err->message);
    g_clear_error(&err);
  }
  g_task_return_boolean(task, ctx->commit_res == BT_CREATE_COMMIT_OK);
}

static void on_create_note_task_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  (void)src; (void)res;
  GTask *task = G_TASK(res);
  CreateTaskCtx *ctx = g_task_get_task_data(task);
  (void)ud;
  if (ctx == NULL || ctx->app_ref == NULL || ctx->app_ref->app == NULL)
    return;

  BtApp *app = ctx->app_ref->app;
  app->create_busy = FALSE;
  if (app->create_popover != NULL)
    bt_createpopover_close(app->create_popover);

  const gchar *cur_active = bt_app_active_note(app);
  gboolean still_on_source = (g_strcmp0(cur_active, ctx->source_note) == 0);

  if (ctx->commit_res == BT_CREATE_COMMIT_OK) {
    if (app->vault != NULL)
      vault_rescan(app->vault, NULL, NULL);
    if (app->tree != NULL && app->vault != NULL)
      bt_filetree_populate(app->tree, app->vault);
    if (app->index != NULL && app->vault != NULL && ctx->committed_relpath != NULL)
      bt_index_upsert_after_create(app->index, app->vault, ctx->committed_relpath, NULL);
    rebuild_link_snapshot(app);
    schedule_graph_refresh_idle(app);

    if (still_on_source && ctx->committed_relpath != NULL) {
      if (bt_app_open_note(app, ctx->committed_relpath, NULL)) {
        if (ctx->anchor != NULL && app->editor != NULL) {
          gdouble ty = 0.0;
          if (bt_editorpane_scroll_to_anchor(app->editor, ctx->anchor, &ty) &&
              !app->nav_replaying && app->nav != NULL) {
            bt_nav_history_update_current(app->nav, ctx->committed_relpath,
                                          bt_editorpane_caret_byte_offset(app->editor), ty);
          }
        }
      }
    }
    set_statusbar_notice_timed(app,
                               ctx->message ? ctx->message : "Created note", 4u);
  } else if (ctx->commit_res == BT_CREATE_COMMIT_EXISTS) {
    if (app->vault != NULL)
      vault_rescan(app->vault, NULL, NULL);
    if (app->index != NULL && app->vault != NULL && ctx->committed_relpath != NULL)
      bt_index_upsert_after_create(app->index, app->vault, ctx->committed_relpath, NULL);
    rebuild_link_snapshot(app);
    if (still_on_source && ctx->committed_relpath != NULL)
      bt_app_open_note(app, ctx->committed_relpath, NULL);
    set_statusbar_notice_timed(app,
                               ctx->message ? ctx->message : "Note already existed, opened it", 6u);
  } else {
    set_statusbar_notice_timed(app,
                               ctx->message ? ctx->message : "Create failed", 6u);
  }
}

static void on_create_popover_confirm(const BtCreatePlan *plan, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->vault == NULL || plan == NULL || app->create_busy)
    return;
  app->create_busy = TRUE;

  CreateTaskCtx *ctx = g_new0(CreateTaskCtx, 1);
  ctx->app_ref = bt_app_ref_acquire(app->self_ref);
  ctx->vault = app->vault;
  ctx->plan = clone_create_plan(plan);
  ctx->source_note = g_strdup(bt_app_active_note(app));
  ctx->anchor = g_strdup(plan->anchor);

  GTask *task = g_task_new(NULL, NULL, on_create_note_task_done, NULL);
  g_task_set_task_data(task, ctx, create_task_ctx_free);
  g_task_run_in_thread(task, create_note_worker);
  g_object_unref(task);
}

static void on_create_popover_cancel(gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL)
    return;
  app->create_busy = FALSE;
}

gpointer bt_app_create_popover_for_test(const BtApp *app)
{
  return app ? app->create_popover : NULL;
}

static gchar *chooser_display_target(const gchar *raw_target)
{
  if (raw_target == NULL)
    return g_strdup("");
  const gchar *pipe = strchr(raw_target, '|');
  gsize len = pipe ? (gsize)(pipe - raw_target) : strlen(raw_target);
  const gchar *hash = memchr(raw_target, '#', len);
  if (hash != NULL)
    len = (gsize)(hash - raw_target);
  const gchar *caret = memchr(raw_target, '^', len);
  if (caret != NULL)
    len = (gsize)(caret - raw_target);
  while (len > 0 && g_ascii_isspace(raw_target[len - 1]))
    len--;
  return g_strndup(raw_target, len);
}

static void on_chooser_open(const gchar *relpath, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->vault == NULL || relpath == NULL)
    return;

  g_autofree gchar *missing_msg = NULL;
  if (!bt_chooser_candidate_exists(app->vault, relpath, &missing_msg)) {
    set_statusbar_notice_timed(app,
                               missing_msg ? missing_msg : "That note no longer exists", 6u);
    if (app->index != NULL)
      bt_index_delete_path(app->index, relpath, NULL);
    vault_rescan(app->vault, NULL, NULL);
    if (app->tree != NULL)
      bt_filetree_populate(app->tree, app->vault);
    rebuild_link_snapshot(app);
    schedule_graph_refresh_idle(app);
    g_clear_pointer(&app->pending_chooser_anchor, g_free);
    return;
  }

  g_autofree gchar *anchor = g_strdup(app->pending_chooser_anchor);
  g_clear_pointer(&app->pending_chooser_anchor, g_free);

  g_autoptr(GError) nerr = NULL;
  if (!bt_app_open_note(app, relpath, &nerr)) {
    set_statusbar_notice_timed(app, nerr ? nerr->message : "Cannot open note", 6u);
    return;
  }
  if (anchor != NULL && app->editor != NULL) {
    gdouble ty = 0.0;
    if (bt_editorpane_scroll_to_anchor(app->editor, anchor, &ty) &&
        !app->nav_replaying && app->nav != NULL) {
      bt_nav_history_update_current(app->nav, relpath,
                                    bt_editorpane_caret_byte_offset(app->editor), ty);
    }
  }
  set_statusbar_notice_timed(app, "", 0u);
}

static void on_chooser_copy(const gchar *link_text, const gchar *status_msg, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || link_text == NULL)
    return;
  g_free(app->last_copied_link);
  app->last_copied_link = g_strdup(link_text);

  GtkWidget *w = app->editor ? bt_editorpane_view_widget(app->editor)
                             : (app->window ? GTK_WIDGET(app->window) : NULL);
  GdkDisplay *dpy = w ? gtk_widget_get_display(w) : gdk_display_get_default();
  GdkClipboard *cb = dpy ? gdk_display_get_clipboard(dpy) : NULL;
  if (cb != NULL)
    gdk_clipboard_set_text(cb, link_text);

  set_statusbar_notice_timed(app, status_msg ? status_msg : "Copied link", 3u);
}

static void on_chooser_cancel(gpointer ud)
{
  BtApp *app = ud;
  if (app != NULL)
    g_clear_pointer(&app->pending_chooser_anchor, g_free);
}

gpointer bt_app_chooser_popover_for_test(const BtApp *app)
{
  return app ? app->chooser_popover : NULL;
}

const gchar *bt_app_last_copied_link_for_test(const BtApp *app)
{
  return app ? app->last_copied_link : NULL;
}

typedef struct {
  BtApp *app;
  gchar *target;
} AsyncOpenCtx;

static void async_open_ctx_free(AsyncOpenCtx *c)
{
  if (!c) return;
  g_free(c->target);
  g_free(c);
}

static void on_file_launcher_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  AsyncOpenCtx *c = ud;
  g_autoptr(GError) err = NULL;
  if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(src), res, &err)) {
    g_autofree gchar *msg = g_strdup_printf("Cannot open '%s': %s",
                                            c->target ? c->target : "",
                                            err ? err->message : "error");
    bt_app_set_statusbar_notice(c->app, msg);
  } else {
    bt_app_set_statusbar_notice(c->app, "");
  }
  async_open_ctx_free(c);
}

static void on_uri_launcher_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  AsyncOpenCtx *c = ud;
  g_autoptr(GError) err = NULL;
  if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err)) {
    g_autofree gchar *msg = g_strdup_printf("Cannot open URL: %s",
                                            err ? err->message : "error");
    bt_app_set_statusbar_notice(c->app, msg);
  } else {
    bt_app_set_statusbar_notice(c->app, "");
  }
  async_open_ctx_free(c);
}

static void execute_permitted_open(BtApp *app, const BtLinkResolution *res)
{
  if (app == NULL || res == NULL)
    return;

  if (res->kind == BT_LINK_KIND_URL) {
    if (res->url == NULL)
      return;
    bt_app_set_statusbar_notice(app, "Opening URL...");
    if (app->test_launcher != NULL) {
      app->test_launcher(res->url, TRUE, app->test_launcher_ud);
      bt_app_set_statusbar_notice(app, "");
      return;
    }
    GtkUriLauncher *ul = gtk_uri_launcher_new(res->url);
    AsyncOpenCtx *ctx = g_new0(AsyncOpenCtx, 1);
    ctx->app = app;
    ctx->target = g_strdup(res->url);
    gtk_uri_launcher_launch(ul, app->window, NULL, on_uri_launcher_done, ctx);
    g_object_unref(ul);
    return;
  }

  if (res->path == NULL || app->vault == NULL)
    return;

  if (res->kind == BT_LINK_KIND_DIR) {
    bt_app_set_statusbar_notice(app, "Opening folder in file manager...");
    if (app->test_launcher != NULL) {
      app->test_launcher(res->path, FALSE, app->test_launcher_ud);
      bt_app_set_statusbar_notice(app, "");
      return;
    }
    reveal_in_file_manager(app, res->path, TRUE);
    bt_app_set_statusbar_notice(app, "");
    return;
  }

  g_autofree gchar *smsg = g_strdup_printf("Opening %s...", res->path);
  bt_app_set_statusbar_notice(app, smsg);
  if (app->test_launcher != NULL) {
    app->test_launcher(res->path, FALSE, app->test_launcher_ud);
    bt_app_set_statusbar_notice(app, "");
    return;
  }

  g_autoptr(GError) rerr = NULL;
  g_autofree gchar *abs = vault_resolve(app->vault, res->path, &rerr);
  if (abs == NULL) {
    bt_app_set_statusbar_notice(app, rerr ? rerr->message : "Blocked: path rejected");
    return;
  }
  GFile *gf = g_file_new_for_path(abs);
  GtkFileLauncher *fl = gtk_file_launcher_new(gf);
  g_object_unref(gf);
  AsyncOpenCtx *ctx = g_new0(AsyncOpenCtx, 1);
  ctx->app = app;
  ctx->target = g_strdup(res->path);
  gtk_file_launcher_launch(fl, app->window, NULL, on_file_launcher_done, ctx);
  g_object_unref(fl);
}

static void on_confirm_open_cancel(GtkButton *b, gpointer ud)
{
  (void)b;
  clear_active_confirm(ud);
}

static void on_confirm_dont_ask_toggled(GtkCheckButton *cb, gpointer ud)
{
  BtApp *app = ud;
  if (app != NULL)
    app->pending_confirm_dont_ask = gtk_check_button_get_active(cb);
}

static BtLinkResolution *dup_resolution(const BtLinkResolution *src)
{
  if (src == NULL) return NULL;
  BtLinkResolution *d = g_new0(BtLinkResolution, 1);
  d->status = src->status;
  d->kind = src->kind;
  d->block_reason = src->block_reason;
  d->path = g_strdup(src->path);
  d->anchor = g_strdup(src->anchor);
  d->block_id = g_strdup(src->block_id);
  d->display = g_strdup(src->display);
  d->url = g_strdup(src->url);
  if (src->candidates != NULL)
    d->candidates = g_strdupv(src->candidates);
  return d;
}

static void on_confirm_open_accept(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_app_confirm_active_popover_for_test(ud, ((BtApp *)ud)->pending_confirm_dont_ask);
}

static void on_confirm_popover_closed(GtkPopover *pop, gpointer ud)
{
  BtApp *app = ud;
  if (app != NULL && app->active_confirm_popover == pop) {
    app->active_confirm_popover = NULL;
    if (gtk_widget_get_parent(GTK_WIDGET(pop)) != NULL)
      gtk_widget_unparent(GTK_WIDGET(pop));
    g_clear_pointer(&app->active_confirm_detail, g_free);
    g_clear_pointer(&app->pending_confirm_res, bt_link_resolution_free);
  }
}

static void show_confirm_open_popover(BtApp *app, const BtLinkResolution *res,
                                      const BtLinkOpenPolicy *pol)
{
  clear_active_confirm(app);
  app->pending_confirm_res = dup_resolution(res);
  app->pending_confirm_dont_ask = FALSE;

  const gchar *title = "Confirm open";
  const gchar *btn_lbl = "Open anyway";
  g_autofree gchar *detail = NULL;

  if (pol->action == BT_LINK_OPEN_CONFIRM_URL) {
    title = "Open external URL?";
    btn_lbl = "Open URL";
    if (pol->has_userinfo) {
      detail = g_strdup_printf("%s\nWarning: embedded credentials were stripped.",
                               pol->safe_display_url ? pol->safe_display_url : res->url);
    } else {
      detail = g_strdup(pol->safe_display_url ? pol->safe_display_url : res->url);
    }
  } else if (pol->action == BT_LINK_OPEN_CONFIRM_DIR) {
    title = "Reveal folder in Explorer?";
    btn_lbl = "Reveal";
    detail = g_strdup(res->path ? res->path : "");
  } else {
    title = "Open unrecognized file type?";
    btn_lbl = "Open anyway";
    detail = g_strdup(res->path ? res->path : "");
  }
  app->active_confirm_detail = g_strdup(detail ? detail : "");

  GtkWidget *parent_w = app->editor ? bt_editorpane_view_widget(app->editor) : GTK_WIDGET(app->window);
  if (parent_w == NULL)
    return;

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_set_parent(pop, parent_w);
  GdkRectangle rect = {40, 40, 4, 18};
  if (app->editor != NULL)
    bt_editorpane_link_popover_rect(app->editor, &rect);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);

  GtkWidget *tl = gtk_label_new(title);
  gtk_widget_add_css_class(tl, "heading");
  gtk_label_set_xalign(GTK_LABEL(tl), 0.0f);
  gtk_box_append(GTK_BOX(box), tl);

  GtkWidget *dl = gtk_label_new(detail);
  gtk_label_set_xalign(GTK_LABEL(dl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(dl), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(dl), 52);
  gtk_widget_add_css_class(dl, "bt-mono");
  gtk_box_append(GTK_BOX(box), dl);

  if (pol->action == BT_LINK_OPEN_CONFIRM_URL) {
    GtkWidget *cb = gtk_check_button_new_with_label("Don't ask again this session");
    g_signal_connect(cb, "toggled", G_CALLBACK(on_confirm_dont_ask_toggled), app);
    gtk_box_append(GTK_BOX(box), cb);
  }

  GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btns, GTK_ALIGN_END);
  GtkWidget *b_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *b_ok = gtk_button_new_with_label(btn_lbl);
  gtk_widget_add_css_class(b_ok, "suggested-action");
  g_signal_connect(b_cancel, "clicked", G_CALLBACK(on_confirm_open_cancel), app);
  g_signal_connect(b_ok, "clicked", G_CALLBACK(on_confirm_open_accept), app);
  gtk_box_append(GTK_BOX(btns), b_cancel);
  gtk_box_append(GTK_BOX(btns), b_ok);
  gtk_box_append(GTK_BOX(box), btns);

  gtk_popover_set_child(GTK_POPOVER(pop), box);
  app->active_confirm_popover = GTK_POPOVER(pop);
  g_signal_connect(pop, "closed", G_CALLBACK(on_confirm_popover_closed), app);
  if (!app->smoke_test && gtk_widget_get_mapped(parent_w))
    gtk_popover_popup(GTK_POPOVER(pop));
}

void bt_app_set_launcher_for_test(BtApp *app, BtLinkLauncherFn launcher, gpointer user_data)
{
  if (app == NULL)
    return;
  app->test_launcher = launcher;
  app->test_launcher_ud = user_data;
}

GtkPopover *bt_app_active_confirm_popover_for_test(const BtApp *app)
{
  return app ? app->active_confirm_popover : NULL;
}

const gchar *bt_app_active_confirm_detail_for_test(const BtApp *app)
{
  return app ? app->active_confirm_detail : NULL;
}

gboolean bt_app_confirm_active_popover_for_test(BtApp *app, gboolean dont_ask_again)
{
  if (app == NULL || app->pending_confirm_res == NULL)
    return FALSE;
  BtLinkResolution *res = dup_resolution(app->pending_confirm_res);
  if (res->kind == BT_LINK_KIND_URL && dont_ask_again)
    app->session_external_confirmed = TRUE;
  clear_active_confirm(app);
  execute_permitted_open(app, res);
  bt_link_resolution_free(res);
  return TRUE;
}

gboolean bt_app_activate_link(BtApp *app, const gchar *raw_target)
{
  g_return_val_if_fail(app != NULL, FALSE);
  if (app->vault == NULL || raw_target == NULL)
    return FALSE;
  if (app->link_snap == NULL)
    rebuild_link_snapshot(app);
  if (app->link_snap == NULL)
    return FALSE;

  /* L3-3: close any open link create/chooser/confirm popover before activating
   * a new link, so at most one link popover is active at a time. An in-progress
   * async create (BUSY) is not interrupted. */
  if (app->create_popover != NULL &&
      bt_createpopover_state(app->create_popover) != BT_CREATE_POPOVER_BUSY)
    bt_createpopover_close(app->create_popover);
  if (app->chooser_popover != NULL && bt_chooserpopover_is_open(app->chooser_popover))
    bt_chooserpopover_close(app->chooser_popover);
  clear_active_confirm(app);

  const gchar *src = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
  BtLinkResolution *res = bt_link_resolve(app->link_snap, src, raw_target);
  if (res == NULL)
    return TRUE;

  if (res->status == BT_LINK_PLACEHOLDER) {
    bt_link_resolution_free(res);
    return TRUE;
  }

  if (res->status == BT_LINK_UNRESOLVED) {
    clear_active_confirm(app);
    if (app->create_popover != NULL &&
        bt_createpopover_state(app->create_popover) == BT_CREATE_POPOVER_BUSY) {
      bt_link_resolution_free(res);
      return FALSE;
    }
    if (app->create_popover != NULL)
      bt_createpopover_close(app->create_popover);

    BtCreatePlan *plan = bt_createnote_plan(app->vault, app->link_snap, src, raw_target);
    gboolean handled = FALSE;
    if (plan->result == BT_CREATE_PLAN_OK) {
      if (app->create_popover == NULL)
        app->create_popover = bt_createpopover_new(on_create_popover_confirm,
                                                   on_create_popover_cancel, app);
      GtkWidget *anchor_w = app->editor ? bt_editorpane_view_widget(app->editor) : NULL;
      if (anchor_w != NULL) {
        GdkRectangle rect = {40, 40, 8, 18};
        bt_editorpane_link_popover_rect(app->editor, &rect);
        bt_createpopover_show(app->create_popover, anchor_w, &rect, plan);
      }
      set_statusbar_notice_timed(app, "", 0);
    } else if (plan->result == BT_CREATE_PLAN_EXISTS) {
      gboolean stale = plan->stale_index;
      if (stale) {
        if (app->vault != NULL)
          vault_rescan(app->vault, NULL, NULL);
        if (app->index != NULL && app->vault != NULL && plan->target_relpath != NULL)
          bt_index_upsert_after_create(app->index, app->vault, plan->target_relpath, NULL);
        rebuild_link_snapshot(app);
        schedule_graph_refresh_idle(app);
      }
      if (plan->target_relpath != NULL && bt_app_open_note(app, plan->target_relpath, NULL)) {
        if (plan->anchor != NULL && app->editor != NULL) {
          gdouble ty = 0.0;
          bt_editorpane_scroll_to_anchor(app->editor, plan->anchor, &ty);
        }
        handled = TRUE;
      }
      if (stale)
        set_statusbar_notice_timed(app, "Index refreshed", 4u);
    } else if (plan->result == BT_CREATE_PLAN_PLACEHOLDER) {
      handled = TRUE;
    } else {
      g_autofree gchar *smsg = bt_createnote_plan_status_message(plan);
      if (smsg != NULL)
        set_statusbar_notice_timed(app, smsg, 6u);
    }
    bt_createnote_plan_free(plan);
    bt_link_resolution_free(res);
    return handled;
  }

  if (res->status == BT_LINK_AMBIGUOUS) {
    BtChooserOpenAction act = bt_chooser_decide_open((const gchar *const *)res->candidates);
    gboolean handled = FALSE;
    if (act == BT_CHOOSER_OPEN_NONE_MATCH) {
      if (app->chooser_popover != NULL)
        bt_chooserpopover_close(app->chooser_popover);
      set_statusbar_notice_timed(app, "No matching notes now", 6u);
    } else if (act == BT_CHOOSER_OPEN_DIRECT) {
      g_autoptr(GError) nerr = NULL;
      handled = bt_app_open_note(app, res->candidates[0], &nerr);
      if (!handled) {
        set_statusbar_notice_timed(app, nerr ? nerr->message : "Cannot open note", 6u);
      } else {
        if (res->anchor != NULL && app->editor != NULL) {
          gdouble ty = 0.0;
          if (bt_editorpane_scroll_to_anchor(app->editor, res->anchor, &ty) &&
              !app->nav_replaying && app->nav != NULL) {
            bt_nav_history_update_current(app->nav, res->candidates[0],
                                          bt_editorpane_caret_byte_offset(app->editor), ty);
          }
        }
        set_statusbar_notice_timed(app, "", 0u);
      }
    } else {
      clear_active_confirm(app);
      if (app->create_popover != NULL &&
          bt_createpopover_state(app->create_popover) != BT_CREATE_POPOVER_BUSY) {
        bt_createpopover_close(app->create_popover);
      }
      g_free(app->pending_chooser_anchor);
      app->pending_chooser_anchor = g_strdup(res->anchor);

      if (app->chooser_popover == NULL)
        app->chooser_popover = bt_chooserpopover_new(on_chooser_open, on_chooser_copy,
                                                     on_chooser_cancel, app);
      GtkWidget *anchor_w = app->editor ? bt_editorpane_view_widget(app->editor) : NULL;
      if (anchor_w != NULL) {
        GdkRectangle rect = {40, 40, 8, 18};
        bt_editorpane_link_popover_rect(app->editor, &rect);
        g_autofree gchar *tgt = chooser_display_target(raw_target);
        bt_chooserpopover_show(app->chooser_popover, anchor_w, &rect, tgt,
                               (const gchar *const *)res->candidates);
      }
      set_statusbar_notice_timed(app, "", 0u);
    }
    bt_link_resolution_free(res);
    return handled;
  }

  gboolean need_url_confirm = app->confirm_external_urls && !app->session_external_confirmed;
  BtLinkOpenPolicy pol;
  bt_link_check_open_policy(res, app->vault, need_url_confirm, &pol);

  gboolean ok = TRUE;
  if (pol.action == BT_LINK_OPEN_BLOCKED) {
    bt_app_set_statusbar_notice(app, pol.reason_msg ? pol.reason_msg : "Blocked");
    ok = FALSE;
  } else if (res->kind == BT_LINK_KIND_SAME_NOTE) {
    if (res->anchor != NULL && app->editor != NULL) {
      gdouble old_scroll = bt_editorpane_scroll_y(app->editor);
      gsize old_caret = bt_editorpane_caret_byte_offset(app->editor);
      gdouble vh = bt_editorpane_viewport_height(app->editor);
      gdouble target_y = old_scroll;
      if (bt_editorpane_scroll_to_anchor(app->editor, res->anchor, &target_y)) {
        gdouble new_scroll = bt_editorpane_scroll_y(app->editor);
        gsize new_caret = bt_editorpane_caret_byte_offset(app->editor);
        if (vh > 0.0 && fabs(new_scroll - old_scroll) >= vh &&
            !app->nav_replaying && app->nav != NULL && src != NULL) {
          bt_nav_history_push_jump(app->nav, src, old_caret, old_scroll,
                                   src, new_caret, new_scroll);
        }
      }
    }
    bt_app_set_statusbar_notice(app, "");
  } else if (res->kind == BT_LINK_KIND_NOTE) {
    g_autoptr(GError) nerr = NULL;
    ok = bt_app_open_note(app, res->path, &nerr);
    if (!ok) {
      bt_app_set_statusbar_notice(app, nerr ? nerr->message : "Cannot open note");
    } else {
      if (res->anchor != NULL && app->editor != NULL) {
        gdouble ty = 0.0;
        if (bt_editorpane_scroll_to_anchor(app->editor, res->anchor, &ty) &&
            !app->nav_replaying && app->nav != NULL) {
          bt_nav_history_update_current(app->nav, res->path,
                                        bt_editorpane_caret_byte_offset(app->editor), ty);
        }
      }
      bt_app_set_statusbar_notice(app, "");
    }
  } else if (pol.action == BT_LINK_OPEN_ALLOW) {
    execute_permitted_open(app, res);
  } else {
    show_confirm_open_popover(app, res, &pol);
  }

  bt_link_open_policy_clear(&pol);
  bt_link_resolution_free(res);
  return ok;
}

void bt_app_set_test_mode(BtApp *app, gboolean enabled)
{
  if (app != NULL)
    app->smoke_test = enabled;
}

void bt_app_prepare_ui_for_test(BtApp *app)
{
  if (app == NULL || app->adw_app == NULL)
    return;
  app->smoke_test = TRUE;
  if (app->window == NULL) {
    on_startup(G_APPLICATION(app->adw_app), app);
    on_activate(NULL, app);
  }
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
    if (app->vault != NULL)
      bt_graphview_set_vault_root(app->graphview, vault_root(app->vault));
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
  gboolean ro_verify = app_is_ro_bench(app);
  BtVault *v = ro_verify ? vault_open_readonly(path, &st, error) : vault_open(path, &st, error);
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
  app->last_checkpoint_oid[0] = '\0';
  if (app->confirm_external_urls_corrupt_pending_log && !ro_verify) {
    bt_vault_log(app->vault, "WARNING",
                 "state.ini invalid [editor] confirm_external_urls; falling back to true");
    app->confirm_external_urls_corrupt_pending_log = FALSE;
  }

  if (!app->smoke_test && app->capture_dir == NULL && !ro_verify) {
    gboolean differs = FALSE;
    bt_agents_apply_vault_template(app->vault, &differs, NULL);
    bt_app_save_last_vault(vault_root(app->vault), NULL);
  }

  if (!ro_verify) {
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
  }

  if (app->editor != NULL)
    bt_editorpane_clear(app->editor);
  if (app->nav != NULL)
    bt_nav_history_clear(app->nav);
  if (app->tree != NULL)
    bt_filetree_populate(app->tree, app->vault);
  rebuild_link_snapshot(app);
  refresh_graph(app, !app->smoke_test && !ro_verify);

  if (app->tabs != NULL && app->capture_dir == NULL &&
      !(app->smoke_test && app->cli_vault == NULL) && !ro_verify) {
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

static void on_graph_notice(const gchar *msg, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->status_graph_notice == NULL)
    return;
  if (msg != NULL && msg[0] != '\0') {
    gtk_label_set_text(GTK_LABEL(app->status_graph_notice), msg);
    gtk_widget_set_visible(app->status_graph_notice, TRUE);
  } else {
    gtk_widget_set_visible(app->status_graph_notice, FALSE);
  }
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

static void sync_nav_buttons(BtApp *app)
{
  if (app == NULL)
    return;
  app->syncing_nav = TRUE;
  const gchar *cur = app->view_stack ? adw_view_stack_get_visible_child_name(app->view_stack) : "editor";
  if (app->seg_editor != NULL)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->seg_editor), g_strcmp0(cur, "sessions") != 0);
  if (app->seg_sessions != NULL)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->seg_sessions), g_strcmp0(cur, "sessions") == 0);
  if (app->seg_graph != NULL)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->seg_graph), app->graph_visible);
  app->syncing_nav = FALSE;
}

static void set_term_visible(BtApp *app, gboolean vis)
{
  GtkWidget *target = app->term_card ? app->term_card : bt_termtabs_widget(app->tabs);
  if (target != NULL)
    gtk_widget_set_visible(target, vis);
}

static void set_graph_visible(BtApp *app, gboolean vis, gboolean persist)
{
  app->graph_visible = vis;
  if (vis)
    ensure_view_stack_visible(app);
  GtkWidget *target = app->graph_card ? app->graph_card :
                      (app->graphview ? bt_graphview_widget(app->graphview) : NULL);
  if (target != NULL)
    gtk_widget_set_visible(target, vis);
  sync_nav_buttons(app);
  if (vis && app->graphview != NULL) {
    if (!bt_graphview_has_graph(app->graphview))
      refresh_graph(app, !app->smoke_test);
    else
      gtk_widget_queue_draw(bt_graphview_widget(app->graphview));
  }
  if (persist && !app->smoke_test)
    save_graph_visible(vis);
}

static void on_seg_editor_toggled(GtkToggleButton *btn, gpointer ud)
{
  BtApp *app = ud;
  if (app->syncing_nav)
    return;
  if (gtk_toggle_button_get_active(btn) && app->view_stack != NULL) {
    ensure_view_stack_visible(app);
    adw_view_stack_set_visible_child_name(app->view_stack, "editor");
  }
  sync_nav_buttons(app);
}

static void on_seg_sessions_toggled(GtkToggleButton *btn, gpointer ud)
{
  BtApp *app = ud;
  if (app->syncing_nav)
    return;
  if (gtk_toggle_button_get_active(btn) && app->view_stack != NULL) {
    ensure_view_stack_visible(app);
    adw_view_stack_set_visible_child_name(app->view_stack, "sessions");
  }
  sync_nav_buttons(app);
}

static void on_seg_graph_toggled(GtkToggleButton *btn, gpointer ud)
{
  BtApp *app = ud;
  if (app->syncing_nav)
    return;
  set_graph_visible(app, gtk_toggle_button_get_active(btn), TRUE);
}

static void on_view_stack_page_changed(GObject *obj, GParamSpec *pspec, gpointer ud)
{
  (void)pspec;
  BtApp *app = ud;
  const gchar *name = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(obj));
  sync_nav_buttons(app);
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
  if (app->editor != NULL && bt_editorpane_is_dirty(app->editor)) {
    if (!bt_editorpane_flush_save(app->editor)) {
      g_set_error(error, BT_ERROR, BT_ERROR_IO, "Save failed — navigation aborted");
      bt_app_set_statusbar_notice(app, "Save failed — navigation aborted");
      return FALSE;
    }
  }

  gchar *text = NULL;
  gsize len = 0;
  if (!vault_read(app->vault, relpath, &text, &len, error))
    return FALSE;

  if (!g_utf8_validate(text, (gssize)len, NULL)) {
    g_free(text);
    g_set_error(error, BT_ERROR, BT_ERROR_ENCODING, "Note '%s' is not valid UTF-8", relpath);
    return FALSE;
  }

  if (!app->nav_replaying && app->nav != NULL) {
    const gchar *prev_rel = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
    gsize prev_caret = app->editor ? bt_editorpane_caret_byte_offset(app->editor) : 0;
    gdouble prev_scroll = app->editor ? bt_editorpane_scroll_y(app->editor) : 0.0;
    bt_nav_history_push_jump(app->nav, prev_rel, prev_caret, prev_scroll, relpath, 0, 0.0);
  }

  bt_editorpane_load(app->editor, relpath, text, len);
  bt_filetree_select_path(app->tree, relpath);
  update_header(app);
  g_free(text);
  return TRUE;
}

static gboolean nav_entry_exists_in_vault(BtVault *vault, const gchar *relpath)
{
  if (vault == NULL || relpath == NULL || relpath[0] == '\0')
    return FALSE;
  g_autofree gchar *abs = vault_resolve(vault, relpath, NULL);
  return abs != NULL && g_file_test(abs, G_FILE_TEST_IS_REGULAR);
}

static gboolean navigate_history_step(BtApp *app, gint dir)
{
  g_return_val_if_fail(app != NULL, FALSE);
  if (app->vault == NULL || app->nav == NULL || app->editor == NULL)
    return FALSE;

  const gchar *cur_rel = bt_editorpane_relpath(app->editor);
  gsize cur_caret = bt_editorpane_caret_byte_offset(app->editor);
  gdouble cur_scroll = bt_editorpane_scroll_y(app->editor);
  if (cur_rel != NULL)
    bt_nav_history_update_current(app->nav, cur_rel, cur_caret, cur_scroll);

  if (dir < 0 && !bt_nav_history_can_go_back(app->nav))
    return FALSE;
  if (dir > 0 && !bt_nav_history_can_go_forward(app->nav))
    return FALSE;

  /* Flush dirty edits BEFORE mutating history cursor so write errors abort cleanly. */
  if (bt_editorpane_is_dirty(app->editor) && !bt_editorpane_flush_save(app->editor)) {
    bt_app_set_statusbar_notice(app, "Save failed — navigation aborted");
    return FALSE;
  }

  gint idx = bt_nav_history_cursor(app->nav) + dir;
  while (idx >= 0 && (gsize)idx < bt_nav_history_count(app->nav)) {
    const BtNavEntry *ent = bt_nav_history_at(app->nav, (gsize)idx);
    if (ent == NULL || !nav_entry_exists_in_vault(app->vault, ent->relpath)) {
      bt_nav_history_remove_at(app->nav, (gsize)idx);
      if (dir < 0)
        idx--;
      continue;
    }
    g_autofree gchar *target_rel = g_strdup(ent->relpath);
    gsize target_caret = ent->caret_offset;
    gdouble target_scroll = ent->scroll_y;
    gboolean need_reload = (g_strcmp0(cur_rel, target_rel) != 0);

    app->nav_replaying = TRUE;
    gboolean ok = TRUE;
    if (need_reload) {
      GError *err = NULL;
      ok = bt_app_open_note(app, target_rel, &err);
      if (!ok) {
        bt_app_set_statusbar_notice(app, err ? err->message : "Cannot open note");
        g_clear_error(&err);
      }
    }
    app->nav_replaying = FALSE;

    if (!ok) {
      bt_nav_history_remove_at(app->nav, (gsize)idx);
      if (dir < 0)
        idx--;
      continue;
    }

    bt_nav_history_set_cursor(app->nav, idx);
    bt_editorpane_set_caret_byte_offset(app->editor, target_caret);
    bt_editorpane_set_scroll_y(app->editor, target_scroll);
    return TRUE;
  }
  return FALSE;
}

gboolean bt_app_navigate_back(BtApp *app)
{
  return navigate_history_step(app, -1);
}

gboolean bt_app_navigate_forward(BtApp *app)
{
  return navigate_history_step(app, 1);
}

BtNavHistory *bt_app_nav_history(const BtApp *app)
{
  return app ? app->nav : NULL;
}

gboolean bt_app_save_active(BtApp *app, GError **error)
{
  g_return_val_if_fail(app != NULL, FALSE);
  if (app->editor != NULL && !bt_editorpane_flush_save(app->editor)) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Save failed");
    return FALSE;
  }
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
    g_strlcpy(app->last_checkpoint_oid, oid, sizeof(app->last_checkpoint_oid));
    update_statusbar(app);
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
  set_term_visible(app, TRUE);
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
  if (app->graphview != NULL && relpath != NULL)
    bt_graphview_select_relpath(app->graphview, relpath);
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
  set_term_visible(app, TRUE);
}

static gboolean on_editor_save(const gchar *relpath, const gchar *text, gsize length, gpointer ud)
{
  BtApp *app = ud;
  if (app->vault == NULL)
    return FALSE;
  if (app->fail_next_save_for_test) {
    app->fail_next_save_for_test = FALSE;
    bt_app_set_statusbar_notice(app, "Save failed — kept in buffer");
    return FALSE;
  }
  GError *err = NULL;
  if (vault_atomic_write(app->vault, relpath, text, length, NULL, &err)) {
    bt_editorpane_mark_saved(app->editor);
    if (app->index != NULL)
      bt_index_upsert_note(app->index, app->vault, relpath, NULL, NULL);
    rebuild_link_snapshot(app);
    refresh_graph(app, FALSE);
    update_header(app);
    gchar *msg = g_strdup_printf("Saved — %s", relpath);
    show_toast(app, msg, 2);
    g_free(msg);
    return TRUE;
  } else {
    gchar *emsg = g_strdup_printf("Save failed: %s", err ? err->message : "io error");
    bt_app_set_statusbar_notice(app, emsg);
    g_free(emsg);
    if (app->toast_overlay != NULL && !app->smoke_test) {
      AdwToast *t = adw_toast_new("Save FAILED — kept in buffer");
      adw_toast_set_button_label(t, "Retry");
      adw_toast_set_action_name(t, "app.save-active");
      adw_toast_overlay_add_toast(app->toast_overlay, t);
    }
    g_clear_error(&err);
    return FALSE;
  }
}

static void on_editor_link(const gchar *target, gpointer ud)
{
  bt_app_activate_link((BtApp *)ud, target);
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
  set_term_visible(app, TRUE);
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
    rebuild_link_snapshot(app);
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
    rebuild_link_snapshot(app);
    refresh_graph(app, FALSE);
    return;
  }

  gboolean skipped = FALSE;
  if (app->index)
    bt_index_upsert_note(app->index, app->vault, relpath, &skipped, NULL);
  vault_rescan(app->vault, NULL, NULL);
  if (app->tree) bt_filetree_populate(app->tree, app->vault);
  rebuild_link_snapshot(app);
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

static void on_graph_select_note(const gchar *relpath, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->tree == NULL || relpath == NULL)
    return;
  bt_filetree_select_path(app->tree, relpath);
}

static void on_graph_open_note(const gchar *relpath, gboolean is_ghost, gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->vault == NULL || relpath == NULL)
    return;
  if (is_ghost) {
    g_autofree gchar *existing = bt_wikilink_find_note(vault_notes(app->vault), relpath);
    if (existing != NULL) {
      GError *err = NULL;
      if (!bt_app_open_note(app, existing, &err) && err != NULL) {
        bt_app_show_error(app, "Cannot open graph node", err->message);
        g_error_free(err);
      }
    } else {
      set_statusbar_notice_timed(app, "Note not found", 4u);
    }
    return;
  }
  GError *err = NULL;
  if (!bt_app_open_note(app, relpath, &err) && err != NULL) {
    bt_app_show_error(app, "Cannot open graph node", err->message);
    g_error_free(err);
  }
}

/* --------------------------------- note file management & context menus -- */

static gchar *normalize_note_relpath(const gchar *input, const gchar *default_dir)
{
  if (input == NULL)
    return NULL;
  gchar *clean = g_strdup(input);
  g_strstrip(clean);
  if (clean[0] == '\0') {
    g_free(clean);
    return NULL;
  }
  for (gchar *c = clean; *c != '\0'; c++) {
    if (*c == '\\')
      *c = '/';
  }
  const gchar *p = clean;
  while (*p == '/')
    p++;
  if (p[0] == '\0') {
    g_free(clean);
    return NULL;
  }

  gchar *joined = NULL;
  if (strchr(p, '/') == NULL && default_dir != NULL && default_dir[0] != '\0' &&
      strcmp(default_dir, ".") != 0) {
    joined = g_strdup_printf("%s/%s", default_dir, p);
  } else {
    joined = g_strdup(p);
  }
  g_free(clean);

  gsize jlen = strlen(joined);
  if (jlen < 3 || g_ascii_strcasecmp(joined + jlen - 3, ".md") != 0) {
    gchar *with_ext = g_strconcat(joined, ".md", NULL);
    g_free(joined);
    joined = with_ext;
  }

  const gchar *slash = strrchr(joined, '/');
  const gchar *base = slash ? (slash + 1) : joined;
  if (strlen(base) <= 3 || base[0] == '.') {
    g_free(joined);
    return NULL;
  }
  return joined;
}

static gchar *yaml_quote_title(const gchar *raw)
{
  GString *out = g_string_new("\"");
  for (const gchar *p = raw ? raw : ""; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c(out, '\\');
    g_string_append_c(out, *p);
  }
  g_string_append_c(out, '"');
  return g_string_free(out, FALSE);
}

static gboolean rename_note_in_app(BtApp *app, const gchar *old_relpath,
                                   const gchar *raw_new_name, GError **error)
{
  if (app == NULL || app->vault == NULL || old_relpath == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "No vault is currently open");
    return FALSE;
  }

  gchar *old_dir = strchr(old_relpath, '/') ? g_path_get_dirname(old_relpath) : NULL;
  gchar *new_rel = normalize_note_relpath(raw_new_name, old_dir);
  g_free(old_dir);
  if (new_rel == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID,
                "Enter a valid note name (for example: my-note.md)");
    return FALSE;
  }

  if (strcmp(old_relpath, new_rel) == 0) {
    g_free(new_rel);
    return TRUE;
  }

  gboolean was_open =
      (app->editor != NULL && g_strcmp0(bt_editorpane_relpath(app->editor), old_relpath) == 0);
  if (was_open && bt_editorpane_is_dirty(app->editor))
    bt_editorpane_flush_save(app->editor);

  if (!vault_rename_note(app->vault, old_relpath, new_rel, error)) {
    g_free(new_rel);
    return FALSE;
  }

  if (app->index != NULL) {
    bt_index_delete_path(app->index, old_relpath, NULL);
    bt_index_upsert_note(app->index, app->vault, new_rel, NULL, NULL);
  }
  vault_rescan(app->vault, NULL, NULL);
  if (app->tree != NULL)
    bt_filetree_populate(app->tree, app->vault);
  rebuild_link_snapshot(app);

  if (was_open) {
    gchar *text = NULL;
    gsize len = 0;
    if (vault_read(app->vault, new_rel, &text, &len, NULL)) {
      bt_editorpane_load(app->editor, new_rel, text, len);
      bt_editorpane_hide_conflict(app->editor);
      g_free(text);
    }
  }
  if (app->tree != NULL)
    bt_filetree_select_path(app->tree, new_rel);

  refresh_graph(app, FALSE);
  update_header(app);

  gchar *msg = g_strdup_printf("Renamed to %s", new_rel);
  show_toast(app, msg, 2);
  g_free(msg);
  g_free(new_rel);
  return TRUE;
}

static gboolean create_named_note_in_app(BtApp *app, const gchar *parent_dir,
                                         const gchar *raw_name, GError **error)
{
  if (app == NULL || app->vault == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "No vault is currently open");
    return FALSE;
  }

  gchar *new_rel = normalize_note_relpath(raw_name, parent_dir);
  if (new_rel == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID,
                "Enter a valid note name (for example: my-note.md)");
    return FALSE;
  }

  gchar *abs = vault_resolve(app->vault, new_rel, error);
  if (abs == NULL) {
    g_free(new_rel);
    return FALSE;
  }
  gboolean exists = g_file_test(abs, G_FILE_TEST_EXISTS);
  g_free(abs);
  if (exists) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "A note named '%s' already exists", new_rel);
    g_free(new_rel);
    return FALSE;
  }

  gchar *base = g_path_get_basename(new_rel);
  gsize blen = strlen(base);
  gchar *stem = (blen > 3) ? g_strndup(base, blen - 3) : g_strdup(base);
  g_free(base);

  GDateTime *now = g_date_time_new_now_local();
  gchar *id_ts = g_date_time_format(now, "%Y%m%d-%H%M%S");
  gchar *iso = g_date_time_format_iso8601(now);
  g_date_time_unref(now);

  gchar *qtitle = yaml_quote_title(stem);
  gchar *content = g_strdup_printf(
      "---\n"
      "id: bt-%s\n"
      "title: %s\n"
      "created: %s\n"
      "updated: %s\n"
      "source: human\n"
      "tags: []\n"
      "---\n\n"
      "# %s\n\n",
      id_ts ? id_ts : "note", qtitle, iso ? iso : "", iso ? iso : "", stem);
  g_free(id_ts);
  g_free(iso);
  g_free(qtitle);
  g_free(stem);

  gboolean ok = vault_atomic_write(app->vault, new_rel, content, strlen(content), NULL, error);
  g_free(content);
  if (!ok) {
    g_free(new_rel);
    return FALSE;
  }

  if (app->index != NULL)
    bt_index_upsert_note(app->index, app->vault, new_rel, NULL, NULL);
  vault_rescan(app->vault, NULL, NULL);
  if (app->tree != NULL)
    bt_filetree_populate(app->tree, app->vault);
  rebuild_link_snapshot(app);
  refresh_graph(app, TRUE);
  bt_app_open_note(app, new_rel, NULL);

  gchar *msg = g_strdup_printf("Created %s", new_rel);
  show_toast(app, msg, 2);
  g_free(msg);
  g_free(new_rel);
  return TRUE;
}

static void duplicate_note_in_app(BtApp *app, const gchar *relpath)
{
  if (app == NULL || app->vault == NULL || relpath == NULL)
    return;
  if (app->editor != NULL && g_strcmp0(bt_editorpane_relpath(app->editor), relpath) == 0 &&
      bt_editorpane_is_dirty(app->editor)) {
    bt_editorpane_flush_save(app->editor);
  }

  gchar *text = NULL;
  gsize len = 0;
  GError *err = NULL;
  if (!vault_read(app->vault, relpath, &text, &len, &err)) {
    bt_app_show_error(app, "Cannot duplicate note", err->message);
    g_error_free(err);
    return;
  }

  gsize rlen = strlen(relpath);
  gchar *stem = (rlen > 3 && g_ascii_strcasecmp(relpath + rlen - 3, ".md") == 0)
                    ? g_strndup(relpath, rlen - 3)
                    : g_strdup(relpath);

  gchar *dest = NULL;
  for (guint n = 1; n <= 99; n++) {
    gchar *cand = (n == 1) ? g_strdup_printf("%s-copy.md", stem)
                           : g_strdup_printf("%s-copy-%u.md", stem, n);
    gchar *abs = vault_resolve(app->vault, cand, NULL);
    gboolean taken = (abs == NULL || g_file_test(abs, G_FILE_TEST_EXISTS));
    g_free(abs);
    if (!taken) {
      dest = cand;
      break;
    }
    g_free(cand);
  }
  g_free(stem);

  if (dest == NULL) {
    g_free(text);
    bt_app_show_error(app, "Cannot duplicate note", "Too many copies already exist.");
    return;
  }

  if (!vault_atomic_write(app->vault, dest, text, len, NULL, &err)) {
    bt_app_show_error(app, "Cannot duplicate note", err->message);
    g_error_free(err);
    g_free(text);
    g_free(dest);
    return;
  }
  g_free(text);

  if (app->index != NULL)
    bt_index_upsert_note(app->index, app->vault, dest, NULL, NULL);
  vault_rescan(app->vault, NULL, NULL);
  if (app->tree != NULL)
    bt_filetree_populate(app->tree, app->vault);
  rebuild_link_snapshot(app);
  refresh_graph(app, TRUE);
  bt_app_open_note(app, dest, NULL);

  gchar *msg = g_strdup_printf("Duplicated to %s", dest);
  show_toast(app, msg, 2);
  g_free(msg);
  g_free(dest);
}

static void copy_path_to_clipboard(BtApp *app, const gchar *relpath)
{
  if (app == NULL || app->window == NULL || relpath == NULL)
    return;
  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(app->window));
  gdk_clipboard_set_text(cb, relpath);
  gchar *msg = g_strdup_printf("Copied: %s", relpath);
  show_toast(app, msg, 2);
  g_free(msg);
}

static void reveal_in_file_manager(BtApp *app, const gchar *relpath, gboolean is_dir)
{
  if (app == NULL || app->vault == NULL)
    return;
  gchar *abs = (relpath != NULL && relpath[0] != '\0')
                   ? vault_resolve(app->vault, relpath, NULL)
                   : g_strdup(vault_root(app->vault));
  if (abs == NULL)
    return;

#ifdef G_OS_WIN32
  gchar *cmd = (is_dir || relpath == NULL || relpath[0] == '\0')
                   ? g_strdup_printf("explorer.exe \"%s\"", abs)
                   : g_strdup_printf("explorer.exe /select,\"%s\"", abs);
  gunichar2 *wcmd = g_utf8_to_utf16(cmd, -1, NULL, NULL, NULL);
  if (wcmd != NULL) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, (LPWSTR)wcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
    g_free(wcmd);
  }
  g_free(cmd);
#else
  gchar *target = (is_dir || relpath == NULL || relpath[0] == '\0') ? g_strdup(abs)
                                                                    : g_path_get_dirname(abs);
  const gchar *argv[] = {"xdg-open", target, NULL};
  g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
  g_free(target);
#endif
  g_free(abs);
}

/* Modal prompt for Rename Note & New Note. */

typedef struct {
  BtApp *app;
  GtkWindow *win;
  GtkWidget *entry;
  GtkWidget *err_lbl;
  gchar *target_rel; /* old_relpath for rename, parent_dir for new note */
  gboolean is_rename;
  gint select_end_chars;
} BtNameModal;

static void bt_name_modal_free(gpointer ud)
{
  BtNameModal *m = ud;
  if (m == NULL)
    return;
  g_free(m->target_rel);
  g_free(m);
}

static gboolean on_name_modal_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                                  GdkModifierType state, gpointer ud)
{
  (void)c; (void)keycode; (void)state;
  BtNameModal *m = ud;
  if (keyval == GDK_KEY_Escape) {
    gtk_window_close(m->win);
    return TRUE;
  }
  return FALSE;
}

static void on_name_modal_entry_map(GtkWidget *entry, gpointer ud)
{
  BtNameModal *m = ud;
  gtk_widget_grab_focus(entry);
  if (m->is_rename && m->select_end_chars > 0)
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, m->select_end_chars);
}

static void on_name_modal_submit(GtkWidget *w, gpointer ud)
{
  (void)w;
  BtNameModal *m = ud;
  const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(m->entry));
  GError *err = NULL;
  gboolean ok = m->is_rename
                    ? rename_note_in_app(m->app, m->target_rel, txt, &err)
                    : create_named_note_in_app(m->app, m->target_rel, txt, &err);
  if (ok) {
    gtk_window_close(m->win);
  } else {
    gtk_label_set_text(GTK_LABEL(m->err_lbl), err ? err->message : "Invalid file name");
    gtk_widget_set_visible(m->err_lbl, TRUE);
    g_clear_error(&err);
    gtk_widget_grab_focus(m->entry);
  }
}

static void on_name_modal_cancel(GtkButton *b, gpointer ud)
{
  (void)b;
  BtNameModal *m = ud;
  gtk_window_close(m->win);
}

static void show_name_modal(BtApp *app, gboolean is_rename, const gchar *target_rel)
{
  if (app == NULL || app->window == NULL || app->vault == NULL || app->smoke_test)
    return;

  GtkWidget *win = adw_window_new();
  gtk_widget_add_css_class(win, "bt-dialog");
  gtk_window_set_title(GTK_WINDOW(win), is_rename ? "Rename Note" : "New Note");
  gtk_window_set_transient_for(GTK_WINDOW(win), app->window);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(win), 420, -1);

  BtNameModal *m = g_new0(BtNameModal, 1);
  m->app = app;
  m->win = GTK_WINDOW(win);
  m->target_rel = g_strdup(target_rel);
  m->is_rename = is_rename;
  g_object_set_data_full(G_OBJECT(win), "bt-name-modal", m, bt_name_modal_free);

  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *hb = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(box, 14);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  gchar *hint_txt = NULL;
  if (is_rename) {
    gchar *dir = (target_rel && strchr(target_rel, '/')) ? g_path_get_dirname(target_rel) : NULL;
    if (dir != NULL)
      hint_txt = g_strdup_printf("Rename in %s/ (or enter folder/name.md to move)", dir);
    else
      hint_txt = g_strdup("Rename note (enter folder/name.md to move into a folder)");
    g_free(dir);
  } else {
    if (target_rel != NULL && target_rel[0] != '\0')
      hint_txt = g_strdup_printf("Create in %s/ (.md added automatically)", target_rel);
    else
      hint_txt = g_strdup("Create in vault root (.md added automatically, or folder/name)");
  }

  GtkWidget *hint = gtk_label_new(hint_txt);
  g_free(hint_txt);
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_widget_add_css_class(hint, "bt-dialog-hint");
  gtk_box_append(GTK_BOX(box), hint);

  m->entry = gtk_entry_new();
  if (is_rename && target_rel != NULL) {
    gchar *base = g_path_get_basename(target_rel);
    gtk_editable_set_text(GTK_EDITABLE(m->entry), base);
    gsize blen = strlen(base);
    if (blen > 3 && g_ascii_strcasecmp(base + blen - 3, ".md") == 0)
      m->select_end_chars = (gint)g_utf8_strlen(base, (gssize)(blen - 3));
    else
      m->select_end_chars = (gint)g_utf8_strlen(base, -1);
    g_free(base);
  } else {
    gtk_entry_set_placeholder_text(GTK_ENTRY(m->entry), "my-note.md");
  }
  g_signal_connect(m->entry, "activate", G_CALLBACK(on_name_modal_submit), m);
  g_signal_connect(m->entry, "map", G_CALLBACK(on_name_modal_entry_map), m);
  gtk_box_append(GTK_BOX(box), m->entry);

  m->err_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(m->err_lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(m->err_lbl), TRUE);
  gtk_widget_add_css_class(m->err_lbl, "bt-dialog-error");
  gtk_widget_set_visible(m->err_lbl, FALSE);
  gtk_box_append(GTK_BOX(box), m->err_lbl);

  GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btns, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btns, 4);

  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel, "clicked", G_CALLBACK(on_name_modal_cancel), m);

  GtkWidget *ok_btn = gtk_button_new_with_label(is_rename ? "Rename" : "Create");
  gtk_widget_add_css_class(ok_btn, "suggested-action");
  g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_name_modal_submit), m);

  gtk_box_append(GTK_BOX(btns), cancel);
  gtk_box_append(GTK_BOX(btns), ok_btn);
  gtk_box_append(GTK_BOX(box), btns);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_name_modal_key), m);
  gtk_widget_add_controller(win, key);

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), box);
  adw_window_set_content(ADW_WINDOW(win), tv);
  gtk_window_present(GTK_WINDOW(win));
}

static void show_rename_dialog(BtApp *app, const gchar *old_relpath)
{
  if (old_relpath != NULL && old_relpath[0] != '\0')
    show_name_modal(app, TRUE, old_relpath);
}

static void show_new_note_dialog(BtApp *app, const gchar *parent_dir)
{
  show_name_modal(app, FALSE, parent_dir);
}

typedef struct {
  BtApp *app;
  gchar *relpath;
} BtDeleteCtx;

static void on_delete_confirm_response(GObject *src, GAsyncResult *res, gpointer ud)
{
  BtDeleteCtx *ctx = ud;
  int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
  if (choice == 1 && ctx->app->vault != NULL) {
    GError *err = NULL;
    if (!vault_delete_note(ctx->app->vault, ctx->relpath, &err)) {
      bt_app_show_error(ctx->app, "Cannot delete note", err ? err->message : "");
      g_clear_error(&err);
    } else {
      if (ctx->app->index != NULL)
        bt_index_delete_path(ctx->app->index, ctx->relpath, NULL);
      if (ctx->app->editor != NULL &&
          g_strcmp0(bt_editorpane_relpath(ctx->app->editor), ctx->relpath) == 0) {
        bt_editorpane_clear(ctx->app->editor);
      }
      vault_rescan(ctx->app->vault, NULL, NULL);
      if (ctx->app->tree != NULL)
        bt_filetree_populate(ctx->app->tree, ctx->app->vault);
      rebuild_link_snapshot(ctx->app);
      refresh_graph(ctx->app, FALSE);
      update_header(ctx->app);
      gchar *msg = g_strdup_printf("Deleted %s", ctx->relpath);
      show_toast(ctx->app, msg, 2);
      g_free(msg);
    }
  }
  g_free(ctx->relpath);
  g_free(ctx);
}

static void show_delete_confirm_dialog(BtApp *app, const gchar *relpath)
{
  if (app == NULL || app->window == NULL || app->vault == NULL || relpath == NULL ||
      app->smoke_test) {
    return;
  }
  BtDeleteCtx *ctx = g_new0(BtDeleteCtx, 1);
  ctx->app = app;
  ctx->relpath = g_strdup(relpath);

  GtkAlertDialog *dlg = gtk_alert_dialog_new("Delete note?");
  gchar *detail = g_strdup_printf(
      "Permanently delete '%s' from disk? You can restore it only if a git checkpoint exists.",
      relpath);
  gtk_alert_dialog_set_detail(dlg, detail);
  const gchar *const buttons[] = {"Cancel", "Delete", NULL};
  gtk_alert_dialog_set_buttons(dlg, buttons);
  gtk_alert_dialog_set_cancel_button(dlg, 0);
  gtk_alert_dialog_set_default_button(dlg, 0);
  gtk_alert_dialog_choose(dlg, app->window, NULL, on_delete_confirm_response, ctx);
  g_free(detail);
  g_object_unref(dlg);
}

static void on_graph_rename_note(const gchar *relpath, gpointer ud)
{
  show_rename_dialog(ud, relpath);
}

static void on_tree_action(BtFileTreeAction action, const gchar *relpath, gboolean is_dir,
                           gpointer ud)
{
  BtApp *app = ud;
  if (app == NULL || app->vault == NULL)
    return;

  switch (action) {
  case BT_TREE_ACT_OPEN:
    if (!is_dir && relpath != NULL)
      on_tree_select(relpath, app);
    break;
  case BT_TREE_ACT_RENAME:
    if (!is_dir && relpath != NULL)
      show_rename_dialog(app, relpath);
    break;
  case BT_TREE_ACT_DUPLICATE:
    if (!is_dir && relpath != NULL)
      duplicate_note_in_app(app, relpath);
    break;
  case BT_TREE_ACT_DELETE:
    if (!is_dir && relpath != NULL)
      show_delete_confirm_dialog(app, relpath);
    break;
  case BT_TREE_ACT_NEW_NOTE:
    if (relpath == NULL || relpath[0] == '\0') {
      show_new_note_dialog(app, NULL);
    } else if (is_dir) {
      show_new_note_dialog(app, relpath);
    } else {
      gchar *dir = g_path_get_dirname(relpath);
      show_new_note_dialog(app, (dir && strcmp(dir, ".") != 0) ? dir : NULL);
      g_free(dir);
    }
    break;
  case BT_TREE_ACT_OPEN_TERM:
    if (relpath == NULL || relpath[0] == '\0') {
      on_tree_open_term(NULL, app);
    } else if (is_dir) {
      on_tree_open_term(relpath, app);
    } else {
      gchar *dir = g_path_get_dirname(relpath);
      on_tree_open_term((dir && strcmp(dir, ".") != 0) ? dir : NULL, app);
      g_free(dir);
    }
    break;
  case BT_TREE_ACT_COPY_PATH:
    if (relpath != NULL)
      copy_path_to_clipboard(app, relpath);
    break;
  case BT_TREE_ACT_REVEAL:
    reveal_in_file_manager(app, relpath, is_dir);
    break;
  case BT_TREE_ACT_RESCAN:
    g_action_group_activate_action(G_ACTION_GROUP(app->adw_app), "rescan-vault", NULL);
    break;
  }
}

static void on_term_fullscreen(gpointer ud)
{
  BtApp *app = ud;
  app->term_fullscreen = !app->term_fullscreen;
  adw_overlay_split_view_set_show_sidebar(app->split_view, !app->term_fullscreen);
  if (app->top_paned != NULL)
    gtk_widget_set_visible(app->top_paned, !app->term_fullscreen);
  set_term_visible(app, TRUE);
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
  set_term_visible(app, TRUE);
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
  show_new_note_dialog(app, NULL);
}

static void act_rename_active(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL) return;
  const gchar *rel = app->editor ? bt_editorpane_relpath(app->editor) : NULL;
  gchar *tree_sel = (rel == NULL && app->tree != NULL) ? bt_filetree_selected_path(app->tree) : NULL;
  const gchar *target = rel ? rel : tree_sel;
  if (target != NULL)
    show_rename_dialog(app, target);
  g_free(tree_sel);
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
    rebuild_link_snapshot(app);
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
  GtkWidget *tw = app->term_card ? app->term_card : bt_termtabs_widget(app->tabs);
  gboolean vis = !gtk_widget_get_visible(tw);
  if (!vis && app->term_fullscreen)
    on_term_fullscreen(app);
  set_term_visible(app, vis);
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

static void act_toggle_graph_debug(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->graphview != NULL)
    bt_graphview_toggle_debug_overlay(app->graphview);
}

static void act_nav_back(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  bt_app_navigate_back((BtApp *)ud);
}

static void act_nav_forward(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  bt_app_navigate_forward((BtApp *)ud);
}

static void act_links_single_click_state(GSimpleAction *a, GVariant *value, gpointer ud)
{
  (void)a;
  BtApp *app = ud;
  gboolean en = g_variant_get_boolean(value);
  bt_app_set_links_open_on_click(app, en, TRUE);
}

static void act_confirm_external_urls_state(GSimpleAction *a, GVariant *value, gpointer ud)
{
  (void)a;
  BtApp *app = ud;
  gboolean en = g_variant_get_boolean(value);
  bt_app_set_confirm_external_urls(app, en, TRUE);
}

static void act_checkpoint(GSimpleAction *a, GVariant *p, gpointer ud)
{
  (void)a; (void)p;
  BtApp *app = ud;
  if (app->vault == NULL) return;
  gchar oid[BT_OID_HEX_LEN] = {0};
  GError *err = NULL;
  if (bt_checkpoint_take(vault_root(app->vault), "bt-manual checkpoint", oid, &err)) {
    g_strlcpy(app->last_checkpoint_oid, oid, sizeof(app->last_checkpoint_oid));
    update_statusbar(app);
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
                    "Build: " BT_BUILD_STAMP);
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
  {"rename-active", act_rename_active, NULL, NULL, NULL, {0}},
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
  {"toggle-graph-debug", act_toggle_graph_debug, NULL, NULL, NULL, {0}},
  {"nav-back", act_nav_back, NULL, NULL, NULL, {0}},
  {"nav-forward", act_nav_forward, NULL, NULL, NULL, {0}},
  {"links-single-click", NULL, NULL, "true", act_links_single_click_state, {0}},
  {"confirm-external-urls", NULL, NULL, "true", act_confirm_external_urls_state, {0}},
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

static gboolean on_capture_settle_tick(GtkWidget *w, GdkFrameClock *clock, gpointer ud);

static const BtGraphCaptureSpec *current_capture_spec(BtApp *app)
{
  if (app->capture_state == NULL || g_ascii_strcasecmp(app->capture_state, "all") == 0) {
    gsize cnt = 0;
    const BtGraphCaptureSpec *all = bt_graph_capture_states(&cnt);
    return (app->capture_idx < cnt) ? &all[app->capture_idx] : NULL;
  }
  return (app->capture_idx == 0) ? bt_graph_capture_find(app->capture_state) : NULL;
}

static gboolean on_capture_poll(gpointer ud)
{
  BtApp *app = ud;
  const BtGraphCaptureSpec *spec = current_capture_spec(app);
  if (spec == NULL) {
    g_application_quit(G_APPLICATION(app->adw_app));
    return G_SOURCE_REMOVE;
  }
  if (!bt_graphview_is_capture_ready(app->graphview, spec))
    return G_SOURCE_CONTINUE;

  bt_graphview_apply_capture_state(app->graphview, spec);
  app->capture_applied = TRUE;
  app->capture_settle_left = MAX(spec->settle_frames, 2u);
  gtk_widget_add_tick_callback(bt_graphview_widget(app->graphview),
                               on_capture_settle_tick, app, NULL);
  return G_SOURCE_REMOVE;
}

static gboolean on_capture_settle_tick(GtkWidget *w, GdkFrameClock *clock, gpointer ud)
{
  (void)w; (void)clock;
  BtApp *app = ud;
  if (app->capture_settle_left > 0u) {
    app->capture_settle_left--;
    gtk_widget_queue_draw(bt_graphview_widget(app->graphview));
    return G_SOURCE_CONTINUE;
  }

  const BtGraphCaptureSpec *spec = current_capture_spec(app);
  if (spec != NULL) {
    GError *err = NULL;
    if (!bt_graphview_capture_evidence(app->graphview, app->window, app->capture_dir, spec,
                                       BT_BUILD_STAMP, &err)) {
      g_printerr("BT_CAPTURE ERROR state=%s: %s\n", spec->name, err ? err->message : "unknown");
      g_clear_error(&err);
      app->capture_failed = TRUE;
      g_application_quit(G_APPLICATION(app->adw_app));
      return G_SOURCE_REMOVE;
    }
    printf("BT_CAPTURE ok state=%s dir=%s\n", spec->name, app->capture_dir);
    fflush(stdout);
  }

  app->capture_idx++;
  app->capture_applied = FALSE;
  if (current_capture_spec(app) == NULL) {
    g_application_quit(G_APPLICATION(app->adw_app));
    return G_SOURCE_REMOVE;
  }
  g_timeout_add(30, on_capture_poll, app);
  return G_SOURCE_REMOVE;
}

static gboolean capture_widget_png(GtkWindow *win, GtkWidget *widget, const gchar *png_path,
                                   GError **error)
{
  if (win == NULL || widget == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "No window/widget to capture");
    return FALSE;
  }
  int w = gtk_widget_get_width(widget);
  int h = gtk_widget_get_height(widget);
  if (w <= 0 || h <= 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "Widget has zero size (%dx%d)", w, h);
    return FALSE;
  }
  GdkPaintable *paintable = gtk_widget_paintable_new(widget);
  GtkSnapshot *snap = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snap, (double)w, (double)h);
  GskRenderNode *node = gtk_snapshot_free_to_node(snap);
  g_object_unref(paintable);
  if (node == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Empty render node");
    return FALSE;
  }
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(win));
  GdkTexture *tex = renderer ? gsk_renderer_render_texture(renderer, node, NULL) : NULL;
  gsk_render_node_unref(node);
  if (tex == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "gsk_renderer_render_texture returned NULL");
    return FALSE;
  }
  gboolean ok = gdk_texture_save_to_png(tex, png_path);
  g_object_unref(tex);
  if (!ok)
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Failed to save PNG '%s'", png_path);
  return ok;
}

static void compute_timing_stats(const GArray *arr, gdouble *out_avg, gdouble *out_p95,
                                 gdouble *out_max)
{
  guint n = arr ? arr->len : 0;
  if (n == 0) {
    if (out_avg) *out_avg = 0.0;
    if (out_p95) *out_p95 = 0.0;
    if (out_max) *out_max = 0.0;
    return;
  }
  g_autofree gdouble *sorted = g_new(gdouble, n);
  gdouble sum = 0.0;
  gdouble max_v = 0.0;
  for (guint i = 0; i < n; i++) {
    gdouble v = g_array_index(arr, gdouble, i);
    sorted[i] = v;
    sum += v;
    if (v > max_v)
      max_v = v;
  }
  for (guint gap = n / 2u; gap > 0u; gap /= 2u) {
    for (guint i = gap; i < n; i++) {
      gdouble tmp = sorted[i];
      guint j = i;
      while (j >= gap && sorted[j - gap] > tmp) {
        sorted[j] = sorted[j - gap];
        j -= gap;
      }
      sorted[j] = tmp;
    }
  }
  guint p95_idx = (guint)((gdouble)(n - 1u) * 0.95);
  if (out_avg) *out_avg = sum / (gdouble)n;
  if (out_p95) *out_p95 = sorted[p95_idx];
  if (out_max) *out_max = max_v;
}

static gboolean verify_l2_click_target(BtApp *app, const gchar *raw_target)
{
  if (app == NULL || app->editor == NULL || raw_target == NULL)
    return FALSE;
  const GArray *spans = bt_editorpane_link_spans(app->editor);
  if (spans == NULL)
    return FALSE;
  for (guint i = 0; i < spans->len; i++) {
    const BtLinkSpan *sp = &g_array_index(spans, BtLinkSpan, i);
    if (g_strcmp0(sp->raw_target, raw_target) == 0) {
      bt_editorpane_set_caret_byte_offset(app->editor, 0);
      return bt_editorpane_simulate_click(app->editor, sp->start + 2, GDK_BUTTON_PRIMARY, 1, 0.0,
                                          0.0, 0);
    }
  }
  return FALSE;
}

static gboolean on_verify_l2_tick(GtkWidget *w, GdkFrameClock *clock, gpointer ud)
{
  BtApp *app = ud;
  if (app->verify_l2_phase == 0) {
    if (app->verify_l2_settle < 8u || gtk_widget_get_width(w) <= 200) {
      app->verify_l2_settle++;
      gtk_widget_queue_draw(w);
      return G_SOURCE_CONTINUE;
    }
    if (app->verify_l2_settle == 8u) {
      const char *warmup[] = {
          "statistical-statistics-notes/chapter-01-descriptive/03-numerical-description.md",
          "statistical-statistics-notes/chapter-02-probability/01-probability-basics.md",
          "statistical-statistics-notes/chapter-01-descriptive/01-data-types-collection.md",
      };
      for (gsize wi = 0; wi < G_N_ELEMENTS(warmup); wi++)
        bt_app_open_note(app, warmup[wi], NULL);
      GError *err = NULL;
      if (!bt_app_open_note(app, "statistical-statistics-notes/MathStats.md", &err)) {
        g_printerr("BT_VERIFY_L2 ERROR open MathStats.md: %s\n", err ? err->message : "unknown");
        g_clear_error(&err);
        app->capture_failed = TRUE;
        g_application_quit(G_APPLICATION(app->adw_app));
        return G_SOURCE_REMOVE;
      }
      if (app->nav != NULL) {
        bt_nav_history_clear(app->nav);
        bt_nav_history_push_jump(app->nav, NULL, 0, 0.0,
                                 "statistical-statistics-notes/MathStats.md", 0, 0.0);
      }
      app->verify_l2_settle++;
      gtk_widget_queue_draw(w);
      return G_SOURCE_CONTINUE;
    }
    if (app->verify_l2_settle < 12u) {
      app->verify_l2_settle++;
      gtk_widget_queue_draw(w);
      return G_SOURCE_CONTINUE;
    }
    app->verify_l2_frame_ms = g_array_new(FALSE, FALSE, sizeof(gdouble));
    app->verify_l2_work_ms = g_array_new(FALSE, FALSE, sizeof(gdouble));
    app->verify_l2_prev_fc_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    app->verify_l2_phase = 1;
    GtkWidget *ev0 = app->editor ? bt_editorpane_view_widget(app->editor) : NULL;
    gtk_widget_queue_draw(ev0 ? ev0 : w);
    return G_SOURCE_CONTINUE;
  }

  gint64 now_fc_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
  if (app->verify_l2_prev_fc_us > 0 && now_fc_us > app->verify_l2_prev_fc_us) {
    gdouble dt_ms = (gdouble)(now_fc_us - app->verify_l2_prev_fc_us) / 1000.0;
    if (app->verify_l2_frames > 0 && dt_ms < 500.0)
      g_array_append_val(app->verify_l2_frame_ms, dt_ms);
  }
  app->verify_l2_prev_fc_us = now_fc_us;

  gint64 t0_us = g_get_monotonic_time();
  guint f = app->verify_l2_frames;
  if (f == 40) {
    verify_l2_click_target(app, "chapter-01-descriptive/03-numerical-description.md");
  } else if (f == 100) {
    bt_app_navigate_back(app);
  } else if (f == 160) {
    verify_l2_click_target(app, "chapter-02-probability/01-probability-basics.md");
  } else if (f == 220) {
    bt_app_navigate_back(app);
    app->verify_l2_nav_back_ok =
        (g_strcmp0(bt_app_active_note(app), "statistical-statistics-notes/MathStats.md") == 0);
  } else if (f == 280) {
    if (!verify_l2_click_target(app, "chapter-01-descriptive/01-data-types-collection.md")) {
      g_printerr("BT_VERIFY_L2 ERROR click target in MathStats.md failed\n");
      app->capture_failed = TRUE;
    }
  }
  gdouble work_ms = (gdouble)(g_get_monotonic_time() - t0_us) / 1000.0;
  g_array_append_val(app->verify_l2_work_ms, work_ms);

  app->verify_l2_frames++;
  if (app->verify_l2_frames <= 360u && !app->capture_failed) {
    GtkWidget *ev = app->editor ? bt_editorpane_view_widget(app->editor) : NULL;
    gtk_widget_queue_draw(ev ? ev : w);
    return G_SOURCE_CONTINUE;
  }

  const gchar *active = bt_app_active_note(app);
  const gchar *expected =
      "statistical-statistics-notes/chapter-01-descriptive/01-data-types-collection.md";
  if (g_strcmp0(active, expected) != 0) {
    g_printerr("BT_VERIFY_L2 ERROR active='%s' expected='%s'\n",
               active ? active : "(null)", expected);
    app->capture_failed = TRUE;
  }

  g_mkdir_with_parents(app->verify_l2_dir, 0755);
  GError *werr = NULL;
  g_autofree gchar *png_path = g_build_filename(app->verify_l2_dir, "mathstats-click-opened.png", NULL);
  if (!app->capture_failed && !capture_widget_png(app->window, GTK_WIDGET(app->window), png_path, &werr)) {
    g_printerr("BT_VERIFY_L2 ERROR png: %s\n", werr ? werr->message : "unknown");
    g_clear_error(&werr);
    app->capture_failed = TRUE;
  }

  gdouble fc_avg = 0.0, fc_p95 = 0.0, fc_max = 0.0;
  gdouble wk_avg = 0.0, wk_p95 = 0.0, wk_max = 0.0;
  compute_timing_stats(app->verify_l2_frame_ms, &fc_avg, &fc_p95, &fc_max);
  compute_timing_stats(app->verify_l2_work_ms, &wk_avg, &wk_p95, &wk_max);

  cJSON *oj = cJSON_CreateObject();
  cJSON_AddStringToObject(oj, "source_note", "statistical-statistics-notes/MathStats.md");
  cJSON_AddStringToObject(oj, "clicked_link", "[[chapter-01-descriptive/01-data-types-collection.md]]");
  cJSON_AddStringToObject(oj, "opened_note", active ? active : "");
  cJSON_AddBoolToObject(oj, "opened_expected", g_strcmp0(active, expected) == 0);
  cJSON_AddBoolToObject(oj, "nav_back_tested", app->verify_l2_nav_back_ok);
  cJSON_AddNumberToObject(oj, "window_width", gtk_widget_get_width(GTK_WIDGET(app->window)));
  cJSON_AddNumberToObject(oj, "window_height", gtk_widget_get_height(GTK_WIDGET(app->window)));
  cJSON_AddStringToObject(oj, "build_stamp", BT_BUILD_STAMP);
  g_autofree gchar *ojson_path = g_build_filename(app->verify_l2_dir, "mathstats-click-opened.json", NULL);
  char *ostr = cJSON_Print(oj);
  cJSON_Delete(oj);
  if (!g_file_set_contents(ojson_path, ostr ? ostr : "{}", -1, NULL))
    app->capture_failed = TRUE;
  free(ostr);

  cJSON *fj = cJSON_CreateObject();
  cJSON_AddStringToObject(fj, "build_stamp", BT_BUILD_STAMP);
  cJSON_AddNumberToObject(fj, "samples", (double)app->verify_l2_frame_ms->len);
  cJSON_AddNumberToObject(fj, "avg_ms", fc_avg);
  cJSON_AddNumberToObject(fj, "p95_ms", fc_p95);
  cJSON_AddNumberToObject(fj, "max_ms", fc_max);
  cJSON_AddNumberToObject(fj, "work_avg_ms", wk_avg);
  cJSON_AddNumberToObject(fj, "work_p95_ms", wk_p95);
  cJSON_AddNumberToObject(fj, "work_max_ms", wk_max);
  cJSON_AddNumberToObject(fj, "budget_ms", 16.6);
  cJSON_AddBoolToObject(fj, "frame_clock_under_budget", fc_p95 < 16.6);
  cJSON_AddBoolToObject(fj, "work_under_budget", wk_p95 < 16.6);
  cJSON *farr = cJSON_CreateArray();
  for (guint i = 0; i < app->verify_l2_frame_ms->len; i++)
    cJSON_AddItemToArray(farr, cJSON_CreateNumber(g_array_index(app->verify_l2_frame_ms, gdouble, i)));
  cJSON_AddItemToObject(fj, "frame_clock_ms", farr);
  g_autofree gchar *fjson_path = g_build_filename(app->verify_l2_dir, "frame-times-l2.json", NULL);
  char *fstr = cJSON_Print(fj);
  cJSON_Delete(fj);
  if (!g_file_set_contents(fjson_path, fstr ? fstr : "{}", -1, NULL))
    app->capture_failed = TRUE;
  free(fstr);

  printf("BT_VERIFY_L2 ok=%d active=%s samples=%u fc_avg=%.2f fc_p95=%.2f fc_max=%.2f work_avg=%.2f work_p95=%.2f work_max=%.2f\n",
         app->capture_failed ? 0 : 1, active ? active : "", app->verify_l2_frame_ms->len,
         fc_avg, fc_p95, fc_max, wk_avg, wk_p95, wk_max);
  fflush(stdout);
  g_application_quit(G_APPLICATION(app->adw_app));
  return G_SOURCE_REMOVE;
}

static gboolean on_bench_first_open_tick(GtkWidget *w, GdkFrameClock *clock, gpointer ud)
{
  BtApp *app = ud;
  const gchar *target =
      (app->bench_first_open_note != NULL && app->bench_first_open_note[0] != '\0')
          ? app->bench_first_open_note
          : "statistical-statistics-notes/MathStats.md";
  GtkWidget *ev = app->editor ? bt_editorpane_view_widget(app->editor) : NULL;

  if (app->bench_fo_phase == 0) {
    if (app->bench_fo_settle < 8u || gtk_widget_get_width(w) <= 200) {
      app->bench_fo_settle++;
      gtk_widget_queue_draw(w);
      return G_SOURCE_CONTINUE;
    }
    app->bench_fo_prev_fc_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    app->bench_fo_open_start_us = g_get_monotonic_time();
    GError *err = NULL;
    if (!bt_app_open_note(app, target, &err)) {
      g_printerr("BT_BENCH_FIRST_OPEN ERROR cold open '%s': %s\n", target,
                 err ? err->message : "unknown");
      g_clear_error(&err);
      app->capture_failed = TRUE;
      g_application_quit(G_APPLICATION(app->adw_app));
      return G_SOURCE_REMOVE;
    }
    app->bench_fo_cold_open_ms =
        (gdouble)(g_get_monotonic_time() - app->bench_fo_open_start_us) / 1000.0;
    app->bench_fo_cold_frames = g_array_new(FALSE, FALSE, sizeof(gdouble));
    app->bench_fo_warm_frames = g_array_new(FALSE, FALSE, sizeof(gdouble));
    app->bench_fo_phase = 1;
    app->bench_fo_frames = 0;
    gtk_widget_queue_draw(ev ? ev : w);
    return G_SOURCE_CONTINUE;
  }

  gint64 now_fc_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
  gdouble dt_ms = (app->bench_fo_prev_fc_us > 0 && now_fc_us > app->bench_fo_prev_fc_us)
                      ? (gdouble)(now_fc_us - app->bench_fo_prev_fc_us) / 1000.0
                      : 0.0;
  app->bench_fo_prev_fc_us = now_fc_us;

  if (app->bench_fo_phase == 1) {
    if (app->bench_fo_frames == 0) {
      app->bench_fo_cold_first_frame_ms = dt_ms;
      app->bench_fo_cold_wall_frame_ms =
          (gdouble)(g_get_monotonic_time() - app->bench_fo_open_start_us) / 1000.0;
    } else if (dt_ms > 0.0 && dt_ms < 500.0) {
      g_array_append_val(app->bench_fo_cold_frames, dt_ms);
    }
    app->bench_fo_frames++;
    if (app->bench_fo_frames < 16u) {
      gtk_widget_queue_draw(ev ? ev : w);
      return G_SOURCE_CONTINUE;
    }
    const gchar *other = NULL;
    const GPtrArray *notes = app->vault ? vault_notes(app->vault) : NULL;
    if (notes != NULL) {
      for (guint i = 0; i < notes->len; i++) {
        const gchar *cand = g_ptr_array_index(notes, i);
        if (g_strcmp0(cand, target) != 0) {
          other = cand;
          break;
        }
      }
    }
    if (other != NULL)
      bt_app_open_note(app, other, NULL);
    else if (app->editor != NULL)
      bt_editorpane_clear(app->editor);
    app->bench_fo_phase = 2;
    app->bench_fo_settle = 0;
    gtk_widget_queue_draw(ev ? ev : w);
    return G_SOURCE_CONTINUE;
  }

  if (app->bench_fo_phase == 2) {
    if (app->bench_fo_settle < 6u) {
      app->bench_fo_settle++;
      gtk_widget_queue_draw(ev ? ev : w);
      return G_SOURCE_CONTINUE;
    }
    app->bench_fo_prev_fc_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    app->bench_fo_open_start_us = g_get_monotonic_time();
    GError *err = NULL;
    if (!bt_app_open_note(app, target, &err)) {
      g_printerr("BT_BENCH_FIRST_OPEN ERROR warm open '%s': %s\n", target,
                 err ? err->message : "unknown");
      g_clear_error(&err);
      app->capture_failed = TRUE;
      g_application_quit(G_APPLICATION(app->adw_app));
      return G_SOURCE_REMOVE;
    }
    app->bench_fo_warm_open_ms =
        (gdouble)(g_get_monotonic_time() - app->bench_fo_open_start_us) / 1000.0;
    app->bench_fo_phase = 3;
    app->bench_fo_frames = 0;
    gtk_widget_queue_draw(ev ? ev : w);
    return G_SOURCE_CONTINUE;
  }

  if (app->bench_fo_frames == 0) {
    app->bench_fo_warm_first_frame_ms = dt_ms;
    app->bench_fo_warm_wall_frame_ms =
        (gdouble)(g_get_monotonic_time() - app->bench_fo_open_start_us) / 1000.0;
  } else if (dt_ms > 0.0 && dt_ms < 500.0) {
    g_array_append_val(app->bench_fo_warm_frames, dt_ms);
  }
  app->bench_fo_frames++;
  if (app->bench_fo_frames < 16u) {
    gtk_widget_queue_draw(ev ? ev : w);
    return G_SOURCE_CONTINUE;
  }

  gdouble cold_avg = 0.0, cold_p95 = 0.0, cold_max = 0.0;
  gdouble warm_avg = 0.0, warm_p95 = 0.0, warm_max = 0.0;
  compute_timing_stats(app->bench_fo_cold_frames, &cold_avg, &cold_p95, &cold_max);
  compute_timing_stats(app->bench_fo_warm_frames, &warm_avg, &warm_p95, &warm_max);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "build_stamp", BT_BUILD_STAMP);
  cJSON_AddStringToObject(root, "note", target);
  cJSON_AddNumberToObject(root, "cold_open_call_ms", app->bench_fo_cold_open_ms);
  cJSON_AddNumberToObject(root, "cold_first_frame_clock_ms", app->bench_fo_cold_first_frame_ms);
  cJSON_AddNumberToObject(root, "cold_first_frame_wall_ms", app->bench_fo_cold_wall_frame_ms);
  cJSON_AddNumberToObject(root, "cold_subsequent_avg_ms", cold_avg);
  cJSON_AddNumberToObject(root, "cold_subsequent_p95_ms", cold_p95);
  cJSON_AddNumberToObject(root, "cold_subsequent_max_ms", cold_max);
  cJSON_AddNumberToObject(root, "warm_open_call_ms", app->bench_fo_warm_open_ms);
  cJSON_AddNumberToObject(root, "warm_first_frame_clock_ms", app->bench_fo_warm_first_frame_ms);
  cJSON_AddNumberToObject(root, "warm_first_frame_wall_ms", app->bench_fo_warm_wall_frame_ms);
  cJSON_AddNumberToObject(root, "warm_subsequent_avg_ms", warm_avg);
  cJSON_AddNumberToObject(root, "warm_subsequent_p95_ms", warm_p95);
  cJSON_AddNumberToObject(root, "warm_subsequent_max_ms", warm_max);
  g_autofree gchar *dir = g_path_get_dirname(app->bench_first_open_out);
  if (dir != NULL && dir[0] != '\0')
    g_mkdir_with_parents(dir, 0755);
  char *jstr = cJSON_Print(root);
  cJSON_Delete(root);
  if (!g_file_set_contents(app->bench_first_open_out, jstr ? jstr : "{}", -1, NULL))
    app->capture_failed = TRUE;
  free(jstr);

  printf("BT_BENCH_FIRST_OPEN ok=%d note=%s cold_open=%.2f cold_ff=%.2f cold_wall=%.2f warm_open=%.2f warm_ff=%.2f warm_wall=%.2f\n",
         app->capture_failed ? 0 : 1, target, app->bench_fo_cold_open_ms,
         app->bench_fo_cold_first_frame_ms, app->bench_fo_cold_wall_frame_ms,
         app->bench_fo_warm_open_ms, app->bench_fo_warm_first_frame_ms,
         app->bench_fo_warm_wall_frame_ms);
  fflush(stdout);
  g_application_quit(G_APPLICATION(app->adw_app));
  return G_SOURCE_REMOVE;
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
  g_menu_append(s3, "Open links with single click", "app.links-single-click");
  g_menu_append(s3, "Confirm external URLs", "app.confirm-external-urls");
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

static GtkWidget *wrap_in_card(GtkWidget *child, const gchar *name)
{
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if (name != NULL)
    gtk_widget_set_name(card, name);
  gtk_widget_add_css_class(card, "bt-card");
  gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_hexpand(card, TRUE);
  gtk_widget_set_vexpand(card, TRUE);
  gtk_box_append(GTK_BOX(card), child);
  return card;
}

static GtkWidget *make_nav_toggle(const gchar *icon_name, const gchar *label, const gchar *tooltip)
{
  GtkWidget *btn = gtk_toggle_button_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *img = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
  gtk_box_append(GTK_BOX(box), img);
  gtk_box_append(GTK_BOX(box), gtk_label_new(label));
  gtk_button_set_child(GTK_BUTTON(btn), box);
  gtk_widget_set_tooltip_text(btn, tooltip);
  return btn;
}

static void on_window_nav_mouse_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                                        gpointer ud)
{
  (void)n_press; (void)x; (void)y;
  BtApp *app = ud;
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (button == 8) {
    bt_app_navigate_back(app);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  } else if (button == 9) {
    bt_app_navigate_forward(app);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }
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
  int def_w = (app->capture_w > 200) ? app->capture_w : 1280;
  int def_h = (app->capture_h > 200) ? app->capture_h : 800;
  gtk_window_set_default_size(app->window, def_w, def_h);
  g_signal_connect(app->window, "close-request", G_CALLBACK(on_window_close), app);

  GtkGesture *nav_mouse = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(nav_mouse), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(nav_mouse), GTK_PHASE_CAPTURE);
  g_signal_connect(nav_mouse, "pressed", G_CALLBACK(on_window_nav_mouse_pressed), app);
  gtk_widget_add_controller(GTK_WIDGET(app->window), GTK_EVENT_CONTROLLER(nav_mouse));

  /* Header bar: left = sidebar + vault title; right = segmented nav + Agent + menu + Win11 controls */
  GtkWidget *header = adw_header_bar_new();
  gtk_widget_set_name(header, "bt-header");
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), TRUE);
  adw_header_bar_set_decoration_layout(ADW_HEADER_BAR(header), ":minimize,maximize,close");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  GtkWidget *sb_btn = gtk_button_new_from_icon_name("bt-sidebar-symbolic");
  gtk_widget_add_css_class(sb_btn, "bt-iconbtn");
  gtk_widget_set_tooltip_text(sb_btn, "Toggle Sidebar (F9)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(sb_btn), "app.toggle-sidebar");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), sb_btn);

  app->win_title = ADW_WINDOW_TITLE(adw_window_title_new("BrainTerm", "No vault open"));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(app->win_title));

  /* Right side: MenuButton, Agent SplitButton, unified Segmented Control */
  GtkWidget *menu_btn = gtk_menu_button_new();
  gtk_widget_add_css_class(menu_btn, "bt-iconbtn");
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "bt-menu-symbolic");
  gtk_widget_set_tooltip_text(menu_btn, "Menu");
  GMenu *pmenu = build_primary_menu();
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(pmenu));
  g_object_unref(pmenu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_btn);

  app->agent_split = ADW_SPLIT_BUTTON(adw_split_button_new());
  GtkWidget *ab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *ab_img = gtk_image_new_from_icon_name("bt-agent-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(ab_img), 16);
  gtk_box_append(GTK_BOX(ab_box), ab_img);
  gtk_box_append(GTK_BOX(ab_box), gtk_label_new("Agent"));
  adw_split_button_set_child(app->agent_split, ab_box);
  gtk_widget_set_tooltip_text(GTK_WIDGET(app->agent_split), "Launch AI Agent (auto-checkpoint)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(app->agent_split), "app.launch-claude");
  GMenu *amenu = build_agent_menu();
  adw_split_button_set_menu_model(app->agent_split, G_MENU_MODEL(amenu));
  g_object_unref(amenu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(app->agent_split));

  /* Content views */
  app->tree = bt_filetree_new(on_tree_select, on_tree_open_term, app);
  bt_filetree_set_action_cb(app->tree, on_tree_action, app);
  app->editor = bt_editorpane_new(on_editor_save, on_editor_link, on_editor_dirty, app);
  bt_editorpane_set_conflict_cbs(app->editor, on_editor_conflict_diff, on_editor_conflict_reload);
  if (app->capture_dir == NULL && !app_is_ro_bench(app)) {
    bt_app_set_links_open_on_click(app, bt_app_load_links_open_on_click(), FALSE);
    gboolean corrupt_confirm = FALSE;
    gboolean confirm_ext = load_confirm_external_urls_internal(app->vault, &corrupt_confirm);
    if (corrupt_confirm && app->vault == NULL)
      app->confirm_external_urls_corrupt_pending_log = TRUE;
    bt_app_set_confirm_external_urls(app, confirm_ext, FALSE);
  } else {
    bt_app_set_links_open_on_click(app, TRUE, FALSE);
    bt_app_set_confirm_external_urls(app, TRUE, FALSE);
  }
  app->graphview = bt_graphview_new(on_graph_open_note, on_tree_open_term, app);
  bt_graphview_set_rename_cb(app->graphview, on_graph_rename_note, app);
  bt_graphview_set_select_cb(app->graphview, on_graph_select_note, app);
  if (!app->smoke_test && app->capture_dir == NULL && !app_is_ro_bench(app)) {
    bt_graphview_set_show_tags(app->graphview, load_saved_graph_bool("show_tags", FALSE));
    bt_graphview_set_show_ghosts(app->graphview, load_saved_graph_bool("show_ghosts", FALSE));
  }
  bt_graphview_set_prefs_cb(app->graphview, on_graph_prefs_changed, app);
  if (app->graph_debug)
    bt_graphview_set_debug_overlay(app->graphview, TRUE);
  app->tabs = bt_termtabs_new(BT_TERMTABS_MAX_DEFAULT, on_term_fullscreen, on_term_status,
                              on_session_ended, app);
  app->sessions_panel = bt_sessionspanel_new(on_session_resume, on_session_rollback, app);

  app->view_stack = ADW_VIEW_STACK(adw_view_stack_new());
  gtk_widget_set_name(GTK_WIDGET(app->view_stack), "bt-view-stack");
  gtk_widget_set_hexpand(GTK_WIDGET(app->view_stack), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(app->view_stack), TRUE);
  adw_view_stack_add_titled_with_icon(app->view_stack, bt_editorpane_widget(app->editor),
                                      "editor", "Editor", "bt-editor-symbolic");
  adw_view_stack_add_titled_with_icon(app->view_stack, bt_sessionspanel_widget(app->sessions_panel),
                                      "sessions", "Sessions", "bt-sessions-symbolic");

  gchar *saved_page = (app->smoke_test || app_is_ro_bench(app)) ? g_strdup("editor") : bt_app_load_saved_page();
  const gchar *stack_page = (g_strcmp0(saved_page, "sessions") == 0) ? "sessions" : "editor";
  adw_view_stack_set_visible_child_name(app->view_stack, stack_page);

  app->graph_visible = (app->smoke_test || app->capture_dir != NULL || app_is_ro_bench(app)) ? TRUE : load_saved_graph_visible();
  if (g_strcmp0(saved_page, "graph") == 0)
    app->graph_visible = TRUE;

  /* Single segmented control: Editor | Sessions | Graph (Graph toggles side-by-side split) */
  GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_name(seg, "bt-segmented");
  gtk_widget_add_css_class(seg, "bt-segmented");
  app->seg_editor = make_nav_toggle("bt-editor-symbolic", "Editor", "Markdown Editor");
  app->seg_sessions = make_nav_toggle("bt-sessions-symbolic", "Sessions", "Agent Sessions");
  app->seg_graph = make_nav_toggle("bt-graph-symbolic", "Graph", "Toggle Side-by-Side Graph (Ctrl+G)");
  gtk_box_append(GTK_BOX(seg), app->seg_editor);
  gtk_box_append(GTK_BOX(seg), app->seg_sessions);
  gtk_box_append(GTK_BOX(seg), app->seg_graph);
  sync_nav_buttons(app);
  g_signal_connect(app->seg_editor, "toggled", G_CALLBACK(on_seg_editor_toggled), app);
  g_signal_connect(app->seg_sessions, "toggled", G_CALLBACK(on_seg_sessions_toggled), app);
  g_signal_connect(app->seg_graph, "toggled", G_CALLBACK(on_seg_graph_toggled), app);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), seg);

  /* Rounded cards on bg-base with 8px transparent hover-accent paned splitters */
  GtkWidget *left_card = wrap_in_card(GTK_WIDGET(app->view_stack), "bt-editor-card");
  app->graph_card = wrap_in_card(bt_graphview_widget(app->graphview), "bt-graph-card");
  app->term_card = wrap_in_card(bt_termtabs_widget(app->tabs), "bt-term-card");

  app->top_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_name(app->top_paned, "bt-top-paned");
  gtk_widget_add_css_class(app->top_paned, "bt-hsplit");
  gtk_widget_set_hexpand(app->top_paned, TRUE);
  gtk_widget_set_vexpand(app->top_paned, TRUE);
  gtk_paned_set_start_child(GTK_PANED(app->top_paned), left_card);
  gtk_paned_set_end_child(GTK_PANED(app->top_paned), app->graph_card);
  gtk_paned_set_position(GTK_PANED(app->top_paned), 610);
  gtk_paned_set_resize_start_child(GTK_PANED(app->top_paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(app->top_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(app->top_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(app->top_paned), FALSE);
  gtk_widget_set_visible(app->graph_card, app->graph_visible);

  app->right_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name(app->right_paned, "bt-right-paned");
  gtk_widget_add_css_class(app->right_paned, "bt-vsplit");
  gtk_widget_set_hexpand(app->right_paned, TRUE);
  gtk_widget_set_vexpand(app->right_paned, TRUE);
  gtk_widget_set_margin_start(app->right_paned, 8);
  gtk_widget_set_margin_end(app->right_paned, 8);
  gtk_widget_set_margin_bottom(app->right_paned, 8);
  gtk_paned_set_start_child(GTK_PANED(app->right_paned), app->top_paned);
  gtk_paned_set_end_child(GTK_PANED(app->right_paned), app->term_card);
  gtk_paned_set_position(GTK_PANED(app->right_paned), 460);
  gtk_paned_set_resize_start_child(GTK_PANED(app->right_paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(app->right_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(app->right_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(app->right_paned), FALSE);

  /* Collapsible notes sidebar card: header (vault name + new note + collapse) + tree */
  GtkWidget *sb_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(sb_box, "bt-sidebar");
  gtk_widget_add_css_class(sb_box, "bt-card");
  gtk_widget_add_css_class(sb_box, "bt-sidebar");
  gtk_widget_set_overflow(sb_box, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_margin_start(sb_box, 8);
  gtk_widget_set_margin_bottom(sb_box, 8);

  GtkWidget *sb_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(sb_hdr, "bt-sidebar-header");
  app->sidebar_vault_lbl = gtk_label_new("Vault");
  gtk_label_set_xalign(GTK_LABEL(app->sidebar_vault_lbl), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(app->sidebar_vault_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(app->sidebar_vault_lbl, TRUE);
  gtk_widget_add_css_class(app->sidebar_vault_lbl, "bt-vault-name");

  GtkWidget *sb_new = gtk_button_new_from_icon_name("bt-plus-symbolic");
  gtk_widget_add_css_class(sb_new, "bt-iconbtn");
  gtk_widget_set_tooltip_text(sb_new, "New Note");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(sb_new), "app.new-note");

  GtkWidget *sb_col = gtk_button_new_from_icon_name("bt-sidebar-symbolic");
  gtk_widget_add_css_class(sb_col, "bt-iconbtn");
  gtk_widget_set_tooltip_text(sb_col, "Collapse Sidebar (F9)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(sb_col), "app.toggle-sidebar");

  gtk_box_append(GTK_BOX(sb_hdr), app->sidebar_vault_lbl);
  gtk_box_append(GTK_BOX(sb_hdr), sb_new);
  gtk_box_append(GTK_BOX(sb_hdr), sb_col);
  gtk_box_append(GTK_BOX(sb_box), sb_hdr);
  gtk_box_append(GTK_BOX(sb_box), bt_filetree_widget(app->tree));

  app->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  gtk_widget_set_name(GTK_WIDGET(app->split_view), "bt-split-view");
  adw_overlay_split_view_set_sidebar(app->split_view, sb_box);
  adw_overlay_split_view_set_content(app->split_view, app->right_paned);
  adw_overlay_split_view_set_min_sidebar_width(app->split_view, 180.0);
  adw_overlay_split_view_set_max_sidebar_width(app->split_view, 240.0);

  app->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_toast_overlay_set_child(app->toast_overlay, GTK_WIDGET(app->split_view));

  /* Slim 24px bottom status bar: vault name, note count, git checkpoint status */
  GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_name(sbar, "bt-statusbar");
  gtk_widget_add_css_class(sbar, "bt-statusbar");

  GtkWidget *sv_ico = gtk_image_new_from_icon_name("bt-folder-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(sv_ico), 12);
  app->status_vault_lbl = gtk_label_new("No vault");
  gtk_widget_add_css_class(app->status_vault_lbl, "bt-status-strong");
  GtkWidget *s_sep = gtk_label_new("·");
  app->status_notes_lbl = gtk_label_new("0 notes");
  app->status_graph_notice = gtk_label_new("");
  gtk_widget_set_name(app->status_graph_notice, "bt-status-graph-notice");
  gtk_widget_set_visible(app->status_graph_notice, FALSE);
  app->status_notice_lbl = gtk_label_new("");
  gtk_widget_set_name(app->status_notice_lbl, "bt-status-notice");
  gtk_widget_add_css_class(app->status_notice_lbl, "dim-label");
  gtk_widget_set_visible(app->status_notice_lbl, FALSE);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  app->status_git_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *sg_ico = gtk_image_new_from_icon_name("bt-git-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(sg_ico), 12);
  app->status_git_lbl = gtk_label_new("no git checkpoint");
  gtk_box_append(GTK_BOX(app->status_git_box), sg_ico);
  gtk_box_append(GTK_BOX(app->status_git_box), app->status_git_lbl);

  GtkWidget *s_sep2 = gtk_label_new("·");
  GtkWidget *sb_build = gtk_label_new("build " BT_BUILD_STAMP);
  gtk_widget_set_name(sb_build, "bt-status-build");

  gtk_box_append(GTK_BOX(sbar), sv_ico);
  gtk_box_append(GTK_BOX(sbar), app->status_vault_lbl);
  gtk_box_append(GTK_BOX(sbar), s_sep);
  gtk_box_append(GTK_BOX(sbar), app->status_notes_lbl);
  gtk_box_append(GTK_BOX(sbar), app->status_graph_notice);
  gtk_box_append(GTK_BOX(sbar), app->status_notice_lbl);
  gtk_box_append(GTK_BOX(sbar), spacer);
  gtk_box_append(GTK_BOX(sbar), app->status_git_box);
  gtk_box_append(GTK_BOX(sbar), s_sep2);
  gtk_box_append(GTK_BOX(sbar), sb_build);
  bt_graphview_set_notice_cb(app->graphview, on_graph_notice, app);

  GtkWidget *tview = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tview), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tview), GTK_WIDGET(app->toast_overlay));
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tview), sbar);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(app->window), tview);

  /* Apply persisted scheme (dark-first default) before presenting. */
  apply_scheme_runtime(app, (app->smoke_test || app_is_ro_bench(app)) ? BT_SCHEME_DARK : load_saved_scheme(), FALSE);

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
    } else if (!app_is_ro_bench(app) && vault_notes(app->vault)->len > 0) {
      const gchar *first = g_ptr_array_index(vault_notes(app->vault), 0);
      bt_app_open_note(app, first, NULL);
    }
  }
  if (!app->smoke_test && !app_is_ro_bench(app) && g_strcmp0(stack_page, "editor") != 0)
    adw_view_stack_set_visible_child_name(app->view_stack, stack_page);
  if (!app_is_ro_bench(app))
    g_signal_connect(app->view_stack, "notify::visible-child-name",
                     G_CALLBACK(on_view_stack_page_changed), app);
  g_free(saved_page);
  g_free(initial);

  if (app->capture_dir != NULL && app->capture_w > 0 && app->capture_w <= 1100) {
    /* Compact capture size (e.g. 1000x700): let graph widget fill the capture viewport. */
    adw_overlay_split_view_set_show_sidebar(app->split_view, FALSE);
    gtk_widget_set_visible(left_card, FALSE);
    gtk_widget_set_visible(app->term_card, FALSE);
  }
  if (app->smoke_test && app->cli_vault != NULL)
    g_timeout_add(250, on_smoke_idle, app);
  else if (app->smoke_test)
    ; /* Unit test mode via bt_app_prepare_ui_for_test */
  else if (app->capture_dir != NULL)
    g_timeout_add(60, on_capture_poll, app);
  else if (app->verify_l2_dir != NULL)
    gtk_widget_add_tick_callback(GTK_WIDGET(app->window), on_verify_l2_tick, app, NULL);
  else if (app->bench_first_open_out != NULL)
    gtk_widget_add_tick_callback(GTK_WIDGET(app->window), on_bench_first_open_tick, app, NULL);
}

static gint on_handle_local_options(GApplication *gapp, GVariantDict *opts, gpointer ud)
{
  (void)gapp;
  BtApp *app = ud;
  if (g_variant_dict_contains(opts, "version")) {
    printf("BrainTerm 0.1.0 (build %s)\n", BT_BUILD_STAMP);
    fflush(stdout);
    return 0;
  }
  gchar *selftest_links_dir = NULL;
  if (g_variant_dict_lookup(opts, "selftest-links", "s", &selftest_links_dir)) {
    int rc = bt_link_selftest_cli(selftest_links_dir);
    g_free(selftest_links_dir);
    return rc;
  }
  g_printerr("BrainTerm build %s\n", BT_BUILD_STAMP);
  gchar *vault = NULL;
  if (g_variant_dict_lookup(opts, "vault", "s", &vault)) {
    g_free(app->cli_vault);
    app->cli_vault = vault;
  }
  if (g_variant_dict_contains(opts, "smoke-test"))
    app->smoke_test = TRUE;
  if (g_variant_dict_contains(opts, "graph-debug"))
    app->graph_debug = TRUE;
  gchar *cap_dir = NULL;
  if (g_variant_dict_lookup(opts, "graph-capture", "s", &cap_dir)) {
    g_free(app->capture_dir);
    app->capture_dir = cap_dir;
  }
  gchar *vl2_dir = NULL;
  if (g_variant_dict_lookup(opts, "verify-l2-life2", "s", &vl2_dir)) {
    g_free(app->verify_l2_dir);
    app->verify_l2_dir = vl2_dir;
  }
  gchar *bfo_out = NULL;
  if (g_variant_dict_lookup(opts, "bench-first-open", "s", &bfo_out)) {
    g_free(app->bench_first_open_out);
    app->bench_first_open_out = bfo_out;
  }
  gchar *bfo_note = NULL;
  if (g_variant_dict_lookup(opts, "bench-note", "s", &bfo_note)) {
    g_free(app->bench_first_open_note);
    app->bench_first_open_note = bfo_note;
  }
  gchar *cap_state = NULL;
  if (g_variant_dict_lookup(opts, "graph-state", "s", &cap_state)) {
    g_free(app->capture_state);
    app->capture_state = cap_state;
  }
  gchar *cap_sz = NULL;
  if (g_variant_dict_lookup(opts, "graph-size", "s", &cap_sz)) {
    int cw = 0, ch = 0;
    if (sscanf(cap_sz, "%dx%d", &cw, &ch) == 2 && cw > 200 && ch > 200) {
      app->capture_w = cw;
      app->capture_h = ch;
    }
    g_free(cap_sz);
  }
  if (app->capture_dir != NULL && app->capture_state != NULL &&
      g_ascii_strcasecmp(app->capture_state, "all") != 0 &&
      bt_graph_capture_find(app->capture_state) == NULL) {
    g_printerr("Unknown --graph-state '%s'. Valid states: default-fit, hover-hub, hover-small, "
               "selected, zoom2x, 3d, reduce-motion-on, reduce-motion-off, all\n",
               app->capture_state);
    return 1;
  }
  return -1;
}

static void on_startup(GApplication *gapp, gpointer ud)
{
  BtApp *app = ud;
  g_action_map_add_action_entries(G_ACTION_MAP(gapp), app_actions, G_N_ELEMENTS(app_actions), app);

  static const gchar *const save_accels[] = {"<Primary>s", NULL};
  static const gchar *const new_accels[] = {"<Primary>n", NULL};
  static const gchar *const rename_accels[] = {"F2", NULL};
  static const gchar *const quick_accels[] = {"<Primary>p", NULL};
  static const gchar *const ask_accels[] = {"<Primary>e", NULL};
  static const gchar *const open_accels[] = {"<Primary>o", NULL};
  static const gchar *const rescan_accels[] = {"F5", NULL};
  static const gchar *const sidebar_accels[] = {"F9", NULL};
  static const gchar *const graph_accels[] = {"<Primary>g", NULL};
  static const gchar *const graph_dbg_accels[] = {"<Primary><Shift>f", NULL};
  static const gchar *const nav_back_accels[] = {"<Alt>Left", NULL};
  static const gchar *const nav_forward_accels[] = {"<Alt>Right", NULL};
  static const gchar *const term_accels[] = {"<Primary>grave", NULL};
  static const gchar *const quit_accels[] = {"<Primary>q", NULL};
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.save-active", save_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.new-note", new_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.rename-active", rename_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.quick-open", quick_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.ask", ask_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.open-vault", open_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.rescan-vault", rescan_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-sidebar", sidebar_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-graph", graph_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-graph-debug", graph_dbg_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.nav-back", nav_back_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.nav-forward", nav_forward_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.toggle-terminal", term_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app->adw_app), "app.quit", quit_accels);
}

BtApp *bt_app_new(void)
{
  bt_checkpoint_init();
  BtApp *app = g_new0(BtApp, 1);
  bt_agents_detect(&app->agents);
  app->nav = bt_nav_history_new();
  app->links_open_on_click = TRUE;
  app->scheme = BT_SCHEME_DARK;
  app->confirm_external_urls = TRUE;
  app->self_ref = bt_app_ref_new(app);
  app->adw_app = adw_application_new("local.brainterm.App", G_APPLICATION_NON_UNIQUE);

  static const GOptionEntry opts[] = {
    {"vault", 'v', 0, G_OPTION_ARG_STRING, NULL, "Open vault directory", "DIR"},
    {"smoke-test", 0, 0, G_OPTION_ARG_NONE, NULL, "Open UI, verify startup, and exit", NULL},
    {"graph-debug", 0, 0, G_OPTION_ARG_NONE, NULL, "Show Brain Graph frame-time debug overlay", NULL},
    {"graph-capture", 0, 0, G_OPTION_ARG_STRING, NULL, "Capture graph PNG + JSON evidence to DIR and exit", "DIR"},
    {"graph-state", 0, 0, G_OPTION_ARG_STRING, NULL, "Named capture state (default-fit|hover-hub|hover-small|selected|zoom2x|3d|reduce-motion-on|reduce-motion-off|all)", "NAME"},
    {"graph-size", 0, 0, G_OPTION_ARG_STRING, NULL, "Window size WxH for --graph-capture (e.g. 1000x700 or 1920x1080)", "WxH"},
    {"selftest-links", 0, 0, G_OPTION_ARG_STRING, NULL, "Run read-only link resolver self-test on DIR and exit", "DIR"},
    {"verify-l2-life2", 0, 0, G_OPTION_ARG_STRING, NULL, "Run read-only L2 click/nav verification on --vault and write evidence to OUT_DIR", "OUT_DIR"},
    {"bench-first-open", 0, 0, G_OPTION_ARG_STRING, NULL, "Measure read-only cold first-open & warm second-open frame timing on --vault and write JSON to OUT_FILE", "OUT_FILE"},
    {"bench-note", 0, 0, G_OPTION_ARG_STRING, NULL, "Relative note path to benchmark with --bench-first-open", "RELPATH"},
    {"version", 'V', 0, G_OPTION_ARG_NONE, NULL, "Print version and build stamp and exit", NULL},
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
  if (app->self_ref != NULL) {
    app->self_ref->app = NULL;
    bt_app_ref_release(app->self_ref);
    app->self_ref = NULL;
  }
  if (app->status_notice_timer_id != 0) {
    g_source_remove(app->status_notice_timer_id);
    app->status_notice_timer_id = 0;
  }
  if (app->deferred_graph_idle_id != 0) {
    g_source_remove(app->deferred_graph_idle_id);
    app->deferred_graph_idle_id = 0;
  }
  g_clear_pointer(&app->create_popover, bt_createpopover_free);
  g_clear_pointer(&app->chooser_popover, bt_chooserpopover_free);
  g_free(app->pending_chooser_anchor);
  g_free(app->last_copied_link);
  clear_active_confirm(app);
  g_clear_pointer(&app->nav, bt_nav_history_free);
  if (app->editor != NULL)
    bt_editorpane_set_index_snapshot(app->editor, NULL, NULL);
  g_clear_pointer(&app->link_snap, bt_index_snapshot_free);
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
  g_free(app->capture_dir);
  g_free(app->capture_state);
  g_free(app->verify_l2_dir);
  if (app->verify_l2_frame_ms != NULL)
    g_array_free(app->verify_l2_frame_ms, TRUE);
  if (app->verify_l2_work_ms != NULL)
    g_array_free(app->verify_l2_work_ms, TRUE);
  g_free(app->bench_first_open_out);
  g_free(app->bench_first_open_note);
  if (app->bench_fo_cold_frames != NULL)
    g_array_free(app->bench_fo_cold_frames, TRUE);
  if (app->bench_fo_warm_frames != NULL)
    g_array_free(app->bench_fo_warm_frames, TRUE);
  g_free(app->last_statusbar_notice);
  g_clear_object(&app->adw_app);
  g_free(app);
  bt_checkpoint_shutdown();
}

int bt_app_run(BtApp *app, int argc, char **argv)
{
  g_return_val_if_fail(app != NULL, 1);
  int rc = g_application_run(G_APPLICATION(app->adw_app), argc, argv);
  return app->capture_failed ? 1 : rc;
}

const gchar *bt_app_build_stamp(void) { return BT_BUILD_STAMP; }
BtVault *bt_app_vault(const BtApp *app) { return app ? app->vault : NULL; }
GtkWindow *bt_app_window(const BtApp *app) { return app ? app->window : NULL; }
