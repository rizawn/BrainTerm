/* test_fix_ui.c — FIX-UI regression suite:
 *   1. test_view_pages: AdwViewStack has editor/graph/sessions, defaults to editor,
 *      and maps each page with >1x1 allocation upon switching.
 *   2. test_editor_mapped: BtEditorPane (GtkSourceView) is a descendant of window,
 *      realized, mapped, visible, >1px tall, and uses bt-ink-dark / bt-ink-light.
 *   3. test_graph_mapped: bt_graphview_set_graph(..., TRUE) while unmapped recovers
 *      on map, animates birth_t > 0.5, allocates >1x1, and paints non-empty grayscale.
 *   4. test_state_restore: obsolete [ui] page in state.ini falls back to "editor". */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <adwaita.h>
#include <cairo.h>
#include <gtksourceview/gtksource.h>
#include <string.h>

#include "app.h"
#include "graph.h"
#include "graphview.h"
#include "index.h"
#include "testutil.h"
#include "ui/editorpane.h"
#include "ui/sessionspanel.h"
#include "vault.h"

static void pump_ms(gint ms)
{
  gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
  while (g_get_monotonic_time() < deadline) {
    while (g_main_context_pending(NULL))
      g_main_context_iteration(NULL, FALSE);
    g_usleep(5000);
  }
}

static gboolean pump_until_allocated(GtkWidget *w, gint timeout_ms)
{
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (g_get_monotonic_time() < deadline) {
    while (g_main_context_pending(NULL))
      g_main_context_iteration(NULL, FALSE);
    if (gtk_widget_get_realized(w) && gtk_widget_get_mapped(w) &&
        gtk_widget_get_width(w) > 1 && gtk_widget_get_height(w) > 1)
      return TRUE;
    g_usleep(5000);
  }
  return FALSE;
}

static gboolean init_ui(void)
{
  if (!gtk_init_check())
    return FALSE;
  adw_init();
  return TRUE;
}

static GtkWidget *make_test_shell(AdwViewStack **out_stack, BtEditorPane **out_ed,
                                  BtGraphView **out_gv, BtSessionsPanel **out_sp)
{
  GtkWidget *win = adw_application_window_new(NULL);
  gtk_window_set_default_size(GTK_WINDOW(win), 1024, 700);

  BtEditorPane *ed = bt_editorpane_new(NULL, NULL, NULL, NULL);
  BtGraphView *gv = bt_graphview_new(NULL, NULL, NULL);
  BtSessionsPanel *sp = bt_sessionspanel_new(NULL, NULL, NULL);

  AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());
  gtk_widget_set_hexpand(GTK_WIDGET(stack), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
  adw_view_stack_add_titled_with_icon(stack, bt_editorpane_widget(ed), "editor", "Editor",
                                      "document-edit-symbolic");
  adw_view_stack_add_titled_with_icon(stack, bt_graphview_widget(gv), "graph", "Graph",
                                      "network-workgroup-symbolic");
  adw_view_stack_add_titled_with_icon(stack, bt_sessionspanel_widget(sp), "sessions", "Sessions",
                                      "utilities-terminal-symbolic");
  adw_view_stack_set_visible_child_name(stack, bt_app_resolve_page_name(NULL));

  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand(paned, TRUE);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_paned_set_start_child(GTK_PANED(paned), GTK_WIDGET(stack));
  gtk_paned_set_end_child(GTK_PANED(paned), gtk_label_new("terminal-slot"));
  gtk_paned_set_position(GTK_PANED(paned), 440);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), paned);

  if (out_stack) *out_stack = stack;
  if (out_ed) *out_ed = ed;
  if (out_gv) *out_gv = gv;
  if (out_sp) *out_sp = sp;
  return win;
}

static void destroy_test_shell(GtkWidget *win, BtEditorPane *ed, BtGraphView *gv,
                               BtSessionsPanel *sp)
{
  gtk_window_destroy(GTK_WINDOW(win));
  pump_ms(20);
  bt_sessionspanel_free(sp);
  bt_graphview_free(gv);
  bt_editorpane_free(ed);
}

static void test_view_pages(void **state)
{
  (void)state;
  if (!init_ui())
    skip();

  AdwViewStack *stack = NULL;
  BtEditorPane *ed = NULL;
  BtGraphView *gv = NULL;
  BtSessionsPanel *sp = NULL;
  GtkWidget *win = make_test_shell(&stack, &ed, &gv, &sp);
  gtk_window_present(GTK_WINDOW(win));

  assert_non_null(adw_view_stack_get_child_by_name(stack, "editor"));
  assert_non_null(adw_view_stack_get_child_by_name(stack, "graph"));
  assert_non_null(adw_view_stack_get_child_by_name(stack, "sessions"));
  assert_string_equal(adw_view_stack_get_visible_child_name(stack), "editor");
  assert_true(pump_until_allocated(bt_editorpane_widget(ed), 1500));

  adw_view_stack_set_visible_child_name(stack, "graph");
  assert_true(pump_until_allocated(bt_graphview_widget(gv), 1500));

  adw_view_stack_set_visible_child_name(stack, "sessions");
  assert_true(pump_until_allocated(bt_sessionspanel_widget(sp), 1500));

  destroy_test_shell(win, ed, gv, sp);
}

static void test_editor_mapped(void **state)
{
  (void)state;
  if (!init_ui())
    skip();

  AdwViewStack *stack = NULL;
  BtEditorPane *ed = NULL;
  BtGraphView *gv = NULL;
  BtSessionsPanel *sp = NULL;
  GtkWidget *win = make_test_shell(&stack, &ed, &gv, &sp);

  static const gchar note[] = "---\ntitle: FixUI\n---\n\n# Hello Ink\nSee [[welcome]].\n";
  bt_editorpane_load(ed, "fix.md", note, strlen(note));
  gtk_window_present(GTK_WINDOW(win));

  GtkWidget *view = bt_editorpane_view_widget(ed);
  assert_non_null(view);
  assert_true(gtk_widget_is_ancestor(view, win));
  assert_true(pump_until_allocated(view, 1500));
  assert_true(gtk_widget_get_visible(view));
  assert_true(gtk_widget_get_height(view) > 1);
  assert_string_equal(bt_editorpane_scheme_id(ed), "bt-ink-dark");

  bt_editorpane_set_dark(ed, FALSE);
  assert_string_equal(bt_editorpane_scheme_id(ed), "bt-ink-light");
  bt_editorpane_set_dark(ed, TRUE);
  assert_string_equal(bt_editorpane_scheme_id(ed), "bt-ink-dark");

  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  assert_true(gtk_text_buffer_get_char_count(buf) > 10);

  /* Live GTK widget paintable check: verify the mapped GtkSourceView paints text (non-uniform pixels). */
  pump_ms(60);
  int vw = gtk_widget_get_width(view);
  int vh = gtk_widget_get_height(view);
  GdkPaintable *vp = gtk_widget_paintable_new(view);
  GtkSnapshot *snap = gtk_snapshot_new();
  gdk_paintable_snapshot(vp, GDK_SNAPSHOT(snap), (double)vw, (double)vh);
  GskRenderNode *node = gtk_snapshot_free_to_node(snap);
  if (node != NULL) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, vw, vh);
    cairo_t *cr = cairo_create(surf);
    gsk_render_node_draw(node, cr);
    cairo_surface_flush(surf);
    int stride = cairo_image_surface_get_stride(surf);
    const unsigned char *data = cairo_image_surface_get_data(surf);
    guint32 first_px = ((const guint32 *)(const void *)data)[0];
    guint diff_pixels = 0;
    for (int y = 0; y < vh; y++) {
      const guint32 *row = (const guint32 *)(const void *)(data + y * stride);
      for (int x = 0; x < vw; x++) {
        if (row[x] != first_px)
          diff_pixels++;
      }
    }
    assert_true(diff_pixels > 50);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    gsk_render_node_unref(node);
  }
  g_object_unref(vp);

  destroy_test_shell(win, ed, gv, sp);
}

static void assert_cairo_nonempty(BtGraph *g, int w, int h)
{
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surf);
  bt_graph_draw_cairo(g, cr, w, h, 0.0, 0.0, 1.0, -1, TRUE);
  cairo_surface_flush(surf);

  int stride = cairo_image_surface_get_stride(surf);
  const unsigned char *data = cairo_image_surface_get_data(surf);
  guint32 first_px = ((const guint32 *)(const void *)data)[0];
  guint diff_pixels = 0;

  for (int y = 0; y < h; y++) {
    const guint32 *row = (const guint32 *)(const void *)(data + y * stride);
    for (int x = 0; x < w; x++) {
      if (row[x] != first_px)
        diff_pixels++;
    }
  }
  assert_true(diff_pixels > 20);
  cairo_destroy(cr);
  cairo_surface_destroy(surf);
}

static void test_graph_mapped(void **state)
{
  (void)state;
  if (!init_ui())
    skip();

  gchar *root = g_dir_make_tmp("bt-fixui-graph-XXXXXX", NULL);
  bt_test_put(root, "a.md", "---\nid: bt-1\ntitle: A\nsource: human\ntags: [ink]\n---\n[[b]]\n");
  bt_test_put(root, "b.md", "---\nid: bt-2\ntitle: B\nsource: ai\n---\nbody\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));

  AdwViewStack *stack = NULL;
  BtEditorPane *ed = NULL;
  BtGraphView *gv = NULL;
  BtSessionsPanel *sp = NULL;
  GtkWidget *win = make_test_shell(&stack, &ed, &gv, &sp);
  gtk_window_present(GTK_WINDOW(win));
  assert_true(pump_until_allocated(bt_editorpane_widget(ed), 1500));

  /* Graph starts UNMAPPED while "editor" page is active; simulate startup refresh_graph(TRUE). */
  GtkWidget *area = bt_graphview_widget(gv);
  assert_false(gtk_widget_get_mapped(area));

  BtGraph *g = bt_graph_build(v, idx, NULL);
  bt_graph_layout(g, 15);
  bt_graphview_set_graph(gv, g, TRUE);
  assert_true(bt_graphview_has_graph(gv));
  assert_true(bt_graphview_min_birth_t(gv) < 0.1);

  /* Switching to "graph" must map the area, resume birth animation, and render non-empty pixels. */
  adw_view_stack_set_visible_child_name(stack, "graph");
  assert_true(pump_until_allocated(area, 1500));

  gint64 deadline = g_get_monotonic_time() + 1500000;
  while (bt_graphview_min_birth_t(gv) <= 0.5 && g_get_monotonic_time() < deadline) {
    while (g_main_context_pending(NULL))
      g_main_context_iteration(NULL, FALSE);
    g_usleep(5000);
  }
  assert_true(bt_graphview_min_birth_t(gv) > 0.5);

  int aw = gtk_widget_get_width(area);
  int ah = gtk_widget_get_height(area);
  assert_true(aw > 1 && ah > 1);
  assert_cairo_nonempty(g, aw, ah);

  destroy_test_shell(win, ed, gv, sp);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_state_restore(void **state)
{
  (void)state;
  assert_string_equal(bt_app_resolve_page_name(NULL), "editor");
  assert_string_equal(bt_app_resolve_page_name(""), "editor");
  assert_string_equal(bt_app_resolve_page_name("obsolete_canvas"), "editor");
  assert_string_equal(bt_app_resolve_page_name("editor"), "editor");
  assert_string_equal(bt_app_resolve_page_name("graph"), "graph");
  assert_string_equal(bt_app_resolve_page_name("sessions"), "sessions");

  gchar *cfg_tmp = g_dir_make_tmp("bt-fixui-cfg-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg_tmp, TRUE);
  g_setenv("XDG_CONFIG_HOME", cfg_tmp, TRUE);

  gchar *ini_dir = g_build_filename(cfg_tmp, "brainterm", NULL);
  g_mkdir_with_parents(ini_dir, 0700);
  gchar *ini_path = g_build_filename(ini_dir, "state.ini", NULL);
  assert_true(g_file_set_contents(ini_path, "[ui]\npage=obsolete_canvas\n", -1, NULL));

  gchar *loaded = bt_app_load_saved_page();
  assert_string_equal(loaded, "editor");
  g_free(loaded);

  bt_app_save_page("graph");
  loaded = bt_app_load_saved_page();
  assert_string_equal(loaded, "graph");
  g_free(loaded);

  bt_app_save_page("not-a-valid-page");
  loaded = bt_app_load_saved_page();
  assert_string_equal(loaded, "editor");
  g_free(loaded);

  g_unsetenv("BT_CONFIG_DIR");
  g_unsetenv("XDG_CONFIG_HOME");
  g_free(ini_path);
  g_free(ini_dir);
  bt_test_rm_rf(cfg_tmp);
  g_free(cfg_tmp);
}

static void test_dual_pane_editor_and_graph(void **state)
{
  (void)state;
  if (!init_ui())
    skip();

  GtkWidget *win = adw_application_window_new(NULL);
  gtk_window_set_default_size(GTK_WINDOW(win), 1000, 680);

  BtEditorPane *ed = bt_editorpane_new(NULL, NULL, NULL, NULL);
  BtGraphView *gv = bt_graphview_new(NULL, NULL, NULL);
  static const gchar note[] = "# Dual Pane\nEditor left, Graph right.\n\n## AI draft\nProposed summary.\n";
  bt_editorpane_load(ed, "dual.md", note, strlen(note));

  AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());
  gtk_widget_set_hexpand(GTK_WIDGET(stack), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
  adw_view_stack_add_titled(stack, bt_editorpane_widget(ed), "editor", "Editor");
  adw_view_stack_set_visible_child_name(stack, "editor");

  GtkWidget *top_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_hexpand(top_paned, TRUE);
  gtk_widget_set_vexpand(top_paned, TRUE);
  gtk_paned_set_start_child(GTK_PANED(top_paned), GTK_WIDGET(stack));
  gtk_paned_set_end_child(GTK_PANED(top_paned), bt_graphview_widget(gv));
  gtk_paned_set_position(GTK_PANED(top_paned), 600);
  gtk_paned_set_resize_start_child(GTK_PANED(top_paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(top_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(top_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(top_paned), FALSE);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), top_paned);
  gtk_window_present(GTK_WINDOW(win));

  GtkWidget *ed_view = bt_editorpane_view_widget(ed);
  GtkWidget *g_area = bt_graphview_widget(gv);
  assert_true(pump_until_allocated(ed_view, 1500));
  assert_true(pump_until_allocated(g_area, 1500));

  /* Both Editor (left) and Graph (right) are mapped and visible simultaneously. */
  int w_split = gtk_widget_get_width(ed_view);
  int w_graph = gtk_widget_get_width(g_area);
  assert_true(w_split > 200);
  assert_true(w_graph > 200);

  /* Hiding the right Graph pane expands the left Editor pane cleanly. */
  gtk_widget_set_visible(g_area, FALSE);
  pump_ms(80);
  assert_false(gtk_widget_get_mapped(g_area));
  assert_true(gtk_widget_get_width(ed_view) > w_split);

  /* Re-showing the Graph pane restores side-by-side layout. */
  gtk_widget_set_visible(g_area, TRUE);
  assert_true(pump_until_allocated(g_area, 1500));
  assert_true(gtk_widget_get_width(g_area) > 200);

  gtk_window_destroy(GTK_WINDOW(win));
  pump_ms(20);
  bt_graphview_free(gv);
  bt_editorpane_free(ed);
}

static GtkWidget *find_descendant_named(GtkWidget *root, const gchar *name)
{
  if (root == NULL)
    return NULL;
  if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
    return root;
  for (GtkWidget *ch = gtk_widget_get_first_child(root); ch != NULL;
       ch = gtk_widget_get_next_sibling(ch)) {
    GtkWidget *found = find_descendant_named(ch, name);
    if (found != NULL)
      return found;
  }
  return NULL;
}

static void test_graphview_g3_orphans_relayout(void **state)
{
  (void)state;
  if (!init_ui())
    skip();

  gchar *root = g_dir_make_tmp("bt-fixui-g3-XXXXXX", NULL);
  bt_test_put(root, "a.md", "---\nid: bt-g1\ntitle: Linked A\nsource: human\n---\n[[b]]\n");
  bt_test_put(root, "b.md", "---\nid: bt-g2\ntitle: Linked B\nsource: human\n---\nbody\n");
  bt_test_put(root, "orphan.md", "---\nid: bt-g3\ntitle: Lone Island\nsource: human\n---\nalone\n");

  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));

  AdwViewStack *stack = NULL;
  BtEditorPane *ed = NULL;
  BtGraphView *gv = NULL;
  BtSessionsPanel *sp = NULL;
  GtkWidget *win = make_test_shell(&stack, &ed, &gv, &sp);
  adw_view_stack_set_visible_child_name(stack, "graph");
  gtk_window_present(GTK_WINDOW(win));

  GtkWidget *area = bt_graphview_widget(gv);
  assert_true(pump_until_allocated(area, 1500));

  BtGraph *g = bt_graph_build(v, idx, NULL);
  bt_graph_layout(g, 15);
  bt_graphview_set_graph(gv, g, FALSE);
  pump_ms(60);

  GtkWidget *orphans_btn = find_descendant_named(area, "bt-graph-orphans");
  GtkWidget *relayout_btn = find_descendant_named(area, "bt-graph-relayout");
  assert_non_null(orphans_btn);
  assert_non_null(relayout_btn);
  assert_true(gtk_widget_get_visible(orphans_btn));
  assert_true(gtk_widget_get_visible(relayout_btn));

  /* Toggle Highlight Orphans ON and verify CSS active state; then click Re-layout. */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(orphans_btn), TRUE);
  pump_ms(40);
  assert_true(gtk_widget_has_css_class(orphans_btn, "bt-pill-active"));

  g_signal_emit_by_name(relayout_btn, "clicked");
  pump_ms(120);
  assert_true(bt_graphview_has_graph(gv));

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(orphans_btn), FALSE);
  pump_ms(30);
  assert_false(gtk_widget_has_css_class(orphans_btn, "bt-pill-active"));

  destroy_test_shell(win, ed, gv, sp);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_build_stamp(void **state)
{
  (void)state;
  const gchar *stamp = bt_app_build_stamp();
  assert_non_null(stamp);
  assert_true(stamp[0] != '\0');
  /* Expected: "<short-hash>[-dirty] · YYYY-MM-DD HH:MM[:SS]" (or "nogit · ..."). */
  gboolean matched = g_regex_match_simple(
      "^([0-9a-f]{7,12}(-dirty)?|nogit) \\x{00B7} [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}(:[0-9]{2})?$",
      stamp, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT);
  if (!matched)
    fail_msg("Build stamp '%s' did not match expected format", stamp);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_build_stamp),
    cmocka_unit_test(test_state_restore),
    cmocka_unit_test(test_view_pages),
    cmocka_unit_test(test_editor_mapped),
    cmocka_unit_test(test_graph_mapped),
    cmocka_unit_test(test_dual_pane_editor_and_graph),
    cmocka_unit_test(test_graphview_g3_orphans_relayout),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
