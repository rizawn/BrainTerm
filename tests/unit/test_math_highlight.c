/* test_math_highlight.c — Group H tests (Phase M1, §B.2.5 & §B.21):
 *   - Verifies zero emphasis tags inside any math span in BtEditorPane
 *     (both GtkSourceView bt-markdown.lang highlighting AND editorpane live-preview tags).
 *   - Verifies emphasis & links OUTSIDE math still work.
 *   - Reproduces Screenshot 2 (02-bootstrap.md) lines exact regression. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <string.h>

#include "math/mathtok.h"
#include "ui/editorpane.h"

static void test_bootstrap_zero_emphasis_in_math(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }

  BtEditorPane *pane = bt_editorpane_new(NULL, NULL, NULL, NULL);
  assert_string_equal(bt_editorpane_language_id(pane), "bt-markdown");

  const char *doc =
      "Resample with replacement from original data $B$ times:\n"
      "1. For $b = 1, \\ldots, B$: draw sample of size $n$ with replacement\n"
      "2. Compute statistic $\\hat{\\theta}^{*(b)}$ for each sample\n"
      "3. Use empirical distribution of $\\hat{\\theta}^*$ for inference\n"
      "$$(\\hat{\\theta}^*_{(\\alpha/2)}, \\hat{\\theta}^*_{(1-\\alpha/2)})$$\n"
      "- Propose $\\theta^* \\sim q(\\theta^* | \\theta^{(t-1)})$\n"
      "- Compute acceptance ratio: $r = \\frac{f(\\theta^* | x) q(\\theta^{(t-1)} | \\theta^*)}{f(\\theta^{(t-1)} | x) q(\\theta^* | \\theta^{(t-1)})}$\n"
      "- Accept $\\theta^{(t)} = \\theta^*$ with probability $\\min(1, r)$; else $\\theta^{(t)} = \\theta^{(t-1)}$\n";

  bt_editorpane_load(pane, "02-bootstrap.md", doc, strlen(doc));

  GArray *spans = bt_math_scan(doc, strlen(doc), NULL);
  assert_int_equal(spans->len, 11);

  for (guint i = 0; i < spans->len; i++) {
    const BtMathSpan *sp = &g_array_index(spans, BtMathSpan, i);
    g_autofree gchar *formula = g_strndup(doc + sp->start, sp->end - sp->start);
    gboolean has_emph = bt_editorpane_range_has_emphasis_tag(pane, sp->start, sp->end);
    if (has_emph)
      fail_msg("Emphasis tag found inside math span %u: %s", i, formula);
    assert_false(bt_editorpane_offset_has_tag_name(pane, sp->src_start, "bt-italic"));
    assert_false(bt_editorpane_offset_has_tag_name(pane, sp->src_start, "bt-bold"));
    assert_false(bt_editorpane_offset_has_tag_name(pane, sp->src_start, "bt-wikilink"));
    assert_true(bt_editorpane_offset_has_tag_name(pane, sp->src_start, "bt-math"));
  }

  printf("BT_TEST_MATH_HIGHLIGHT 02-bootstrap spans=%u emphasis_inside_math=0\n", spans->len);
  g_array_unref(spans);
  bt_editorpane_free(pane);
}

static void test_emphasis_outside_math_and_underscore_protection(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }

  BtEditorPane *pane = bt_editorpane_new(NULL, NULL, NULL, NULL);
  const char *doc = "**bold** and $a*b*c_d_e$ and *italic* and [[link]] and $[[not_a_link]]$\n";
  bt_editorpane_load(pane, "mixed.md", doc, strlen(doc));

  const char *bold_pos = strstr(doc, "bold");
  const char *italic_pos = strstr(doc, "italic");
  const char *link_pos = strstr(doc, "link]]");
  const char *math1 = strstr(doc, "a*b*c_d_e");
  const char *math2 = strstr(doc, "[[not_a_link]]");

  assert_non_null(bold_pos);
  assert_non_null(italic_pos);
  assert_non_null(link_pos);
  assert_non_null(math1);
  assert_non_null(math2);

  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(bold_pos - doc), "bt-bold"));
  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(italic_pos - doc), "bt-italic"));
  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(link_pos - doc), "bt-wikilink"));

  assert_false(bt_editorpane_range_has_emphasis_tag(
      pane, (gsize)(math1 - doc) - 1, (gsize)(math1 - doc) + strlen("a*b*c_d_e") + 1));
  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(math1 - doc) + 2, "bt-math"));

  assert_false(bt_editorpane_offset_has_tag_name(pane, (gsize)(math2 - doc) + 2, "bt-wikilink"));
  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(math2 - doc) + 2, "bt-math"));

  bt_editorpane_free(pane);
}

static void test_multiline_display_math_and_lang_order(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }

  BtEditorPane *pane = bt_editorpane_new(NULL, NULL, NULL, NULL);
  const char *doc =
      "$$\n"
      "x_1 * y_1 + z_2 * w_2\n"
      "$$\n\n"
      "After *real_italic* text.\n";
  bt_editorpane_load(pane, "multiline.md", doc, strlen(doc));

  const char *inner = strstr(doc, "x_1 * y_1");
  const char *after = strstr(doc, "real_italic");
  assert_false(bt_editorpane_range_has_emphasis_tag(
      pane, (gsize)(inner - doc), (gsize)(inner - doc) + strlen("x_1 * y_1 + z_2 * w_2")));
  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(inner - doc), "bt-math"));
  assert_true(bt_editorpane_offset_has_tag_name(pane, (gsize)(after - doc), "bt-italic"));

  /* Verify bt-markdown.lang lists math-display & math-inline before markdown:markdown. */
  g_autofree gchar *cwd = g_get_current_dir();
  const char *src_root = g_getenv("BT_SOURCE_ROOT");
  if (src_root == NULL || src_root[0] == '\0')
    src_root = cwd;
  assert_non_null(src_root);
  g_autofree gchar *lang_path = g_build_filename(src_root, "data", "language-specs", "bt-markdown.lang", NULL);
  g_autofree gchar *lang_xml = NULL;
  assert_true(g_file_get_contents(lang_path, &lang_xml, NULL, NULL));
  const char *bt_md = strstr(lang_xml, "<context id=\"bt-markdown\">");
  assert_non_null(bt_md);
  const char *p_disp = strstr(bt_md, "<context ref=\"math-display\"/>");
  const char *p_inl = strstr(bt_md, "<context ref=\"math-inline\"/>");
  const char *p_md = strstr(bt_md, "<context ref=\"markdown:markdown\"/>");
  assert_non_null(p_disp);
  assert_non_null(p_inl);
  assert_non_null(p_md);
  assert_true(p_disp < p_md);
  assert_true(p_inl < p_md);

  bt_editorpane_free(pane);
}

static guint gsv_warnings = 0;

static void on_gsv_log(const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer ud)
{
  (void)domain;
  (void)level;
  (void)ud;
  gsv_warnings++;
  g_printerr("Unexpected GtkSourceView warning: %s\n", msg);
}

int main(void)
{
  g_log_set_handler("GtkSourceView", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, on_gsv_log, NULL);
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_bootstrap_zero_emphasis_in_math),
    cmocka_unit_test(test_emphasis_outside_math_and_underscore_protection),
    cmocka_unit_test(test_multiline_display_math_and_lang_order),
  };
  int rc = cmocka_run_group_tests(tests, NULL, NULL);
  if (gsv_warnings > 0)
    return 1;
  return rc;
}
