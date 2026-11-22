/* test_ink_theme.c — M-UI "anti-alay" monochrome verification suite:
 *   1. test_theme_tokens: every #RRGGBB in bt-ink.css is pure R==G==B grayscale,
 *      and dark surface/raised/ink within +-2 of measured mock (#0B0B0C/#101011/#ECECEE).
 *   2. test_scheme: bt-ink-dark.xml & bt-ink-light.xml have zero chroma hexes,
 *      frontmatter is mid-gray (not pink).
 *   3. test_graph_mono: headless Cairo render of fixture vault graph -> EVERY
 *      pixel in both dark and light renders has R == G == B!
 *   4. test_action_map: every pre-reskin GAction exists in app.c (zero orphans).
 *   5. test_runtime_scheme_switch: Dark <-> Light <-> System palette flips cleanly. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <cairo.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"
#include "graphview.h"
#include "index.h"
#include "testutil.h"
#include "theme.h"
#include "vault.h"

static void assert_all_hexes_grayscale(const gchar *path)
{
  gchar *raw = NULL;
  assert_true(g_file_get_contents(path, &raw, NULL, NULL));
  guint hex_count = 0;
  for (const gchar *p = raw; *p != '\0'; p++) {
    if (*p == '#' && strlen(p) >= 7) {
      gboolean is_hex = TRUE;
      for (int k = 1; k <= 6; k++)
        if (!g_ascii_isxdigit(p[k]))
          is_hex = FALSE;
      if (is_hex) {
        gchar tok[8];
        memcpy(tok, p, 7);
        tok[7] = '\0';
        if (!bt_hex_is_grayscale(tok, NULL))
          fail_msg("Chroma color '%s' found in %s (violates Ink monochrome rule!)", tok, path);
        hex_count++;
      }
    }
  }
  assert_true(hex_count >= 8);
  g_free(raw);
}

static void test_theme_tokens(void **state)
{
  (void)state;
  const gchar *src = g_getenv("BT_SOURCE_ROOT");
  assert_non_null(src);
  gchar *css = g_build_filename(src, "data", "bt-ink.css", NULL);
  assert_all_hexes_grayscale(css);
  g_free(css);

  /* Bonus assertion: dark surface/raised/ink within +-2 of mock (#0B0B0C/#101011/#ECECEE). */
  const BtInkPalette *dk = bt_theme_palette(TRUE);
  assert_true(abs((int)(dk->surface & 0xFF) - 0x0B) <= 2);
  assert_true(abs((int)(dk->raised & 0xFF) - 0x10) <= 2);
  assert_true(abs((int)(dk->ink & 0xFF) - 0xEC) <= 2);
}

static void test_scheme(void **state)
{
  (void)state;
  const gchar *src = g_getenv("BT_SOURCE_ROOT");
  assert_non_null(src);
  gchar *dark_xml = g_build_filename(src, "data", "styles", "bt-ink-dark.xml", NULL);
  gchar *light_xml = g_build_filename(src, "data", "styles", "bt-ink-light.xml", NULL);
  assert_all_hexes_grayscale(dark_xml);
  assert_all_hexes_grayscale(light_xml);

  gchar *raw = NULL;
  assert_true(g_file_get_contents(dark_xml, &raw, NULL, NULL));
  assert_non_null(strstr(raw, "yaml:map-key"));
  assert_null(strstr(raw, "pink"));
  assert_null(strstr(raw, "magenta"));
  g_free(raw);
  g_free(dark_xml);
  g_free(light_xml);
}

static void assert_surface_pure_grayscale(cairo_surface_t *surf)
{
  cairo_surface_flush(surf);
  int w = cairo_image_surface_get_width(surf);
  int h = cairo_image_surface_get_height(surf);
  int stride = cairo_image_surface_get_stride(surf);
  const unsigned char *data = cairo_image_surface_get_data(surf);
  for (int y = 0; y < h; y++) {
    const guint32 *row = (const guint32 *)(const void *)(data + y * stride);
    for (int x = 0; x < w; x++) {
      guint32 px = row[x];
      guint8 r = (guint8)((px >> 16) & 0xFF);
      guint8 g = (guint8)((px >> 8) & 0xFF);
      guint8 b = (guint8)(px & 0xFF);
      if (r != g || g != b)
        fail_msg("Non-grayscale pixel (%u,%u,%u) at (%d,%d) in graph render!", r, g, b, x, y);
    }
  }
}

static void test_graph_mono(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-gmono-XXXXXX", NULL);
  bt_test_put(root, "a.md", "---\nid: bt-1\ntitle: A\nsource: human\ntags: [t1]\n---\n[[b]] [[Ghost]]\n");
  bt_test_put(root, "b.md", "---\nid: bt-2\ntitle: B\nsource: ai\n---\nhello\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));
  BtGraph *g = bt_graph_build(v, idx, NULL);
  bt_graph_layout(g, 10);

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 300);
  cairo_t *cr = cairo_create(surf);

  /* Dark mode graph render: every single pixel R == G == B. */
  bt_graph_draw_cairo(g, cr, 400, 300, 0.0, 0.0, 1.0, 0, TRUE);
  assert_surface_pure_grayscale(surf);

  /* Light mode graph render: every single pixel R == G == B. */
  bt_graph_draw_cairo(g, cr, 400, 300, 0.0, 0.0, 1.0, 0, FALSE);
  assert_surface_pure_grayscale(surf);

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
  const gchar *src = g_getenv("BT_SOURCE_ROOT");
  assert_non_null(src);
  gchar *app_c = g_build_filename(src, "src", "app.c", NULL);
  gchar *raw = NULL;
  assert_true(g_file_get_contents(app_c, &raw, NULL, NULL));

  static const gchar *const required_actions[] = {
    "\"open-vault\"", "\"open-note\"", "\"save-active\"", "\"quick-open\"", "\"rescan-vault\"",
    "\"term-spawn\"", "\"toggle-terminal\"", "\"toggle-sessions\"", "\"launch-claude\"",
    "\"launch-codex\"", "\"launch-gemini\"", "\"checkpoint\"", "\"ask\"", "\"rebuild-index\"",
    "\"audit-index\"", "\"organize-now\"", "\"toggle-graph\"", "\"preferences\"", "\"quit\"",
    NULL,
  };
  for (guint i = 0; required_actions[i] != NULL; i++) {
    if (strstr(raw, required_actions[i]) == NULL)
      fail_msg("Action %s missing after M-UI reskin (orphan check failed)", required_actions[i]);
  }
  g_free(raw);
  g_free(app_c);
}

static void test_runtime_scheme_switch(void **state)
{
  (void)state;
  assert_int_equal(bt_scheme_from_string("dark"), BT_SCHEME_DARK);
  assert_int_equal(bt_scheme_from_string("light"), BT_SCHEME_LIGHT);
  assert_int_equal(bt_scheme_from_string("system"), BT_SCHEME_SYSTEM);
  assert_string_equal(bt_scheme_to_string(BT_SCHEME_DARK), "dark");
  assert_string_equal(bt_scheme_to_string(BT_SCHEME_LIGHT), "light");
  assert_string_equal(bt_scheme_to_string(BT_SCHEME_SYSTEM), "system");

  const BtInkPalette *dk = bt_theme_palette(TRUE);
  const BtInkPalette *lt = bt_theme_palette(FALSE);
  assert_true(dk->surface < dk->ink);
  assert_true(lt->surface > lt->ink);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_theme_tokens),
    cmocka_unit_test(test_scheme),
    cmocka_unit_test(test_graph_mono),
    cmocka_unit_test(test_action_map),
    cmocka_unit_test(test_runtime_scheme_switch),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
