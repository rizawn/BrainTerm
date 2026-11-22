/* test_theme.c — "Dusk" design-system verification:
 *   1. test_token_parity: @define-color tokens in data/theme.css == BT_COL_* in theme.h.
 *   2. test_contrast_aa: text colors meet WCAG AA on the surfaces they sit on.
 *   3. test_css_parses: theme.css loads with ZERO GtkCssProvider parse errors.
 *   4. test_ansi_palette: 16 distinct terminal colors derived from the tokens.
 *   5. test_icons_present: every icon in the GResource manifest exists on disk.
 *   6. test_graph_render: headless Cairo render of the fixture graph is non-empty.
 *   7. test_action_map: every GAction survives the redesign.
 *   8. test_scheme_names: scheme string round-trips. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <cairo.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"
#include "graphview.h"
#include "index.h"
#include "testutil.h"
#include "theme.h"
#include "vault.h"

static gchar *read_text(const gchar *root, const gchar *a, const gchar *b)
{
  gchar *path = b ? g_build_filename(root, a, b, NULL) : g_build_filename(root, a, NULL);
  gchar *raw = NULL;
  assert_true(g_file_get_contents(path, &raw, NULL, NULL));
  g_free(path);
  return raw;
}

static const gchar *root_dir(void)
{
  const gchar *src = g_getenv("BT_SOURCE_ROOT");
  assert_non_null(src);
  return src;
}

/* Finds "@define-color <name> #RRGGBB;" and returns the parsed token. */
static guint32 css_token(const gchar *css, const gchar *name)
{
  gchar *needle = g_strdup_printf("@define-color %s", name);
  const gchar *p = strstr(css, needle);
  if (p == NULL)
    fail_msg("token %s missing from theme.css", name);
  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;
  guint32 v = 0;
  if (!bt_theme_parse_hex(p, &v))
    fail_msg("token %s is not a #RRGGBB value", name);
  g_free(needle);
  return v;
}

static void test_token_parity(void **state)
{
  (void)state;
  gchar *css = read_text(root_dir(), "data", "theme.css");
  assert_int_equal(css_token(css, "bt_bg_base"), BT_COL_BG_BASE);
  assert_int_equal(css_token(css, "bt_surface_1"), BT_COL_SURFACE_1);
  assert_int_equal(css_token(css, "bt_surface_2"), BT_COL_SURFACE_2);
  assert_int_equal(css_token(css, "bt_border"), BT_COL_BORDER);
  assert_int_equal(css_token(css, "bt_border_strong"), BT_COL_BORDER_STRONG);
  assert_int_equal(css_token(css, "bt_text_primary"), BT_COL_TEXT_PRIMARY);
  assert_int_equal(css_token(css, "bt_text_secondary"), BT_COL_TEXT_SECONDARY);
  assert_int_equal(css_token(css, "bt_text_muted"), BT_COL_TEXT_MUTED);
  assert_int_equal(css_token(css, "bt_accent"), BT_COL_ACCENT);
  assert_int_equal(css_token(css, "bt_success"), BT_COL_SUCCESS);
  assert_int_equal(css_token(css, "bt_warning"), BT_COL_WARNING);
  assert_int_equal(css_token(css, "bt_danger"), BT_COL_DANGER);
  assert_int_equal(css_token(css, "bt_graph_bg"), BT_COL_GRAPH_BG);
  assert_int_equal(css_token(css, "bt_graph_vignette"), BT_COL_GRAPH_VIGNETTE);
  assert_int_equal(css_token(css, "bt_graph_node_glow"), BT_COL_GRAPH_NODE_GLOW);
  assert_int_equal(css_token(css, "bt_graph_edge_dim"), BT_COL_GRAPH_EDGE_DIM);
  assert_int_equal(css_token(css, "bt_graph_edge_active"), BT_COL_GRAPH_EDGE_ACTIVE);
  assert_int_equal(css_token(css, "bt_graph_dust"), BT_COL_GRAPH_DUST);
  assert_non_null(strstr(css, "alpha(#8B7CF6, 0.14)")); /* accent-soft = 14% */
  g_free(css);
}

static void test_contrast_aa(void **state)
{
  (void)state;
  assert_true(bt_theme_contrast(BT_COL_TEXT_PRIMARY, BT_COL_SURFACE_1) >= 7.0);
  assert_true(bt_theme_contrast(BT_COL_TEXT_PRIMARY, BT_COL_BG_BASE) >= 7.0);
  assert_true(bt_theme_contrast(BT_COL_TEXT_SECONDARY, BT_COL_SURFACE_1) >= 4.5);
  assert_true(bt_theme_contrast(BT_COL_TEXT_SECONDARY, BT_COL_SURFACE_2) >= 4.5);
  assert_true(bt_theme_contrast(BT_COL_TEXT_SECONDARY, BT_COL_BG_BASE) >= 4.5);
  assert_true(bt_theme_contrast(BT_COL_ACCENT, BT_COL_SURFACE_2) >= 4.5);
  assert_true(bt_theme_contrast(BT_COL_ACCENT, BT_COL_SURFACE_1) >= 4.5);
  assert_true(bt_theme_contrast(BT_COL_DANGER, BT_COL_SURFACE_1) >= 4.5);
}

static guint css_errors;

static void on_css_error(GtkCssProvider *p, GtkCssSection *s, GError *e, gpointer ud)
{
  (void)p; (void)ud;
  css_errors++;
  const GtkCssLocation *loc = s ? gtk_css_section_get_start_location(s) : NULL;
  print_message("theme.css:%u: %s\n", loc ? (guint)loc->lines + 1u : 0u, e->message);
}

static void test_css_parses(void **state)
{
  (void)state;
  if (!gtk_init_check())
    skip();
  gchar *path = g_build_filename(root_dir(), "data", "theme.css", NULL);
  GtkCssProvider *prov = gtk_css_provider_new();
  g_signal_connect(prov, "parsing-error", G_CALLBACK(on_css_error), NULL);
  css_errors = 0;
  gtk_css_provider_load_from_path(prov, path);
  assert_int_equal(css_errors, 0);
  g_object_unref(prov);
  g_free(path);
}

static void test_ansi_palette(void **state)
{
  (void)state;
  for (guint i = 0; i < 16; i++)
    for (guint j = i + 1; j < 16; j++)
      assert_true(bt_theme_ansi(i) != bt_theme_ansi(j));
  assert_int_equal(bt_theme_ansi(1), BT_COL_DANGER);
  assert_int_equal(bt_theme_ansi(2), BT_COL_SUCCESS);
  assert_int_equal(bt_theme_ansi(3), BT_COL_WARNING);
  assert_int_equal(bt_theme_ansi(5), BT_COL_ACCENT);
  for (guint i = 1; i < 8; i++)
    assert_true(bt_theme_contrast(bt_theme_ansi(i), BT_COL_SURFACE_1) >= 4.5);
  for (guint i = 9; i < 16; i++)
    assert_true(bt_theme_contrast(bt_theme_ansi(i), BT_COL_SURFACE_1) >= 4.5);
  assert_true(bt_theme_contrast(bt_theme_ansi(8), BT_COL_SURFACE_1) >= 3.0);
}

static void test_icons_present(void **state)
{
  (void)state;
  gchar *xml = read_text(root_dir(), "data", "brainterm.gresource.xml");
  guint n = 0;
  for (const gchar *p = xml; (p = strstr(p, ">icons/")) != NULL; p++) {
    const gchar *end = strstr(p, "</file>");
    assert_non_null(end);
    gchar *rel = g_strndup(p + 1, (gsize)(end - p - 1));
    gchar *full = g_build_filename(root_dir(), "data", rel, NULL);
    if (!g_file_test(full, G_FILE_TEST_IS_REGULAR))
      fail_msg("icon %s listed in gresource but missing", rel);
    gchar *svg = NULL;
    assert_true(g_file_get_contents(full, &svg, NULL, NULL));
    assert_non_null(strstr(svg, "<path"));
    g_free(svg);
    g_free(full);
    g_free(rel);
    n++;
  }
  assert_true(n >= 12);
  g_free(xml);
}

static void test_graph_render(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-theme-graph-XXXXXX", NULL);
  bt_test_put(root, "a.md", "---\nid: bt-1\ntitle: A\nsource: human\ntags: [t1]\n---\n[[b]] [[Ghost]]\n");
  bt_test_put(root, "b.md", "---\nid: bt-2\ntitle: B\nsource: ai\n---\nhello\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));
  BtGraph *g = bt_graph_build(v, idx, NULL);
  bt_graph_layout(g, 10);

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 300);
  cairo_t *cr = cairo_create(surf);
  bt_graph_draw_cairo(g, cr, 400, 300, 0.0, 0.0, 1.0, -1, TRUE);
  cairo_surface_flush(surf);

  const guint32 *px = (const guint32 *)(const void *)cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf) / 4;
  guint32 bg = px[0] & 0x00FFFFFFu;
  assert_int_equal(bg, BT_COL_SURFACE_1); /* graph card background == surface-1 */
  guint differing = 0;
  gboolean saw_accent_family = FALSE;
  for (int y = 0; y < 300; y++)
    for (int x = 0; x < 400; x++) {
      guint32 c = px[y * stride + x] & 0x00FFFFFFu;
      if (c != bg)
        differing++;
      guint r = (c >> 16) & 0xFF, b = c & 0xFF;
      if (b > r + 40)
        saw_accent_family = TRUE;
    }
  assert_true(differing > 200);
  assert_true(saw_accent_family); /* AI node / glow uses the violet accent */

  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  bt_graph_free(g);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_action_map(void **state)
{
  (void)state;
  gchar *raw = read_text(root_dir(), "src", "app.c");
  static const gchar *const required_actions[] = {
    "\"open-vault\"", "\"open-note\"", "\"save-active\"", "\"quick-open\"", "\"rescan-vault\"",
    "\"term-spawn\"", "\"toggle-terminal\"", "\"toggle-sessions\"", "\"launch-claude\"",
    "\"launch-codex\"", "\"launch-gemini\"", "\"checkpoint\"", "\"ask\"", "\"rebuild-index\"",
    "\"audit-index\"", "\"organize-now\"", "\"toggle-graph\"", "\"preferences\"", "\"quit\"",
    NULL,
  };
  for (guint i = 0; required_actions[i] != NULL; i++) {
    if (strstr(raw, required_actions[i]) == NULL)
      fail_msg("Action %s missing after redesign (orphan check failed)", required_actions[i]);
  }
  g_free(raw);
}

static void test_scheme_names(void **state)
{
  (void)state;
  assert_int_equal(bt_scheme_from_string("dark"), BT_SCHEME_DARK);
  assert_int_equal(bt_scheme_from_string("light"), BT_SCHEME_LIGHT);
  assert_int_equal(bt_scheme_from_string("system"), BT_SCHEME_SYSTEM);
  assert_string_equal(bt_scheme_to_string(BT_SCHEME_DARK), "dark");
  assert_string_equal(bt_scheme_to_string(BT_SCHEME_LIGHT), "light");
  assert_string_equal(bt_scheme_to_string(BT_SCHEME_SYSTEM), "system");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_token_parity),
    cmocka_unit_test(test_contrast_aa),
    cmocka_unit_test(test_css_parses),
    cmocka_unit_test(test_ansi_palette),
    cmocka_unit_test(test_icons_present),
    cmocka_unit_test(test_graph_render),
    cmocka_unit_test(test_action_map),
    cmocka_unit_test(test_scheme_names),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
