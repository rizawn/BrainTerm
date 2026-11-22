/* test_editor_links.c — Part A Phase L2 editor link model, gestures, tooltips & nav tests. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "linkresolve.h"
#include "ui/editorpane.h"

static gchar *pane_buffer_text(const BtEditorPane *pane)
{
  GtkTextView *tv = GTK_TEXT_VIEW(bt_editorpane_view_widget((BtEditorPane *)pane));
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(buf, &s, &e);
  return gtk_text_buffer_get_text(buf, &s, &e, TRUE);
}

static void assert_spans_match_full(const BtEditorPane *pane, const char *ctx)
{
  g_autofree gchar *txt = pane_buffer_text(pane);
  GArray *full = bt_link_scan(txt, strlen(txt));
  const GArray *inc = bt_editorpane_link_spans(pane);
  assert_non_null(inc);
  if (inc->len != full->len) {
    fail_msg("%s: incremental len=%u != full len=%u\nText:\n%s",
             ctx, inc->len, full->len, txt);
  }
  for (guint i = 0; i < full->len; i++) {
    const BtLinkSpan *a = &g_array_index(inc, BtLinkSpan, i);
    const BtLinkSpan *b = &g_array_index(full, BtLinkSpan, i);
    if (a->start != b->start || a->end != b->end ||
        a->target_start != b->target_start || a->target_end != b->target_end ||
        a->syntax != b->syntax || a->line_no != b->line_no ||
        g_strcmp0(a->raw_target, b->raw_target) != 0) {
      fail_msg("%s span[%u] mismatch: inc=(%zu..%zu line=%u '%s') vs full=(%zu..%zu line=%u '%s')",
               ctx, i, a->start, a->end, a->line_no, a->raw_target,
               b->start, b->end, b->line_no, b->raw_target);
    }
  }
  /* Verify binary-search lookup returns identical results at midpoint of every span. */
  for (guint i = 0; i < inc->len; i++) {
    const BtLinkSpan *sp = &g_array_index(inc, BtLinkSpan, i);
    BtLinkSpan hit;
    gsize probe = sp->start + (sp->end - sp->start) / 2;
    assert_true(bt_editorpane_link_at_offset(pane, probe, &hit));
    assert_string_equal(hit.raw_target, sp->raw_target);
  }
  g_array_unref(full);
}

static void test_l2_1_incremental_500_seeded_edits(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }

  BtEditorPane *pane = bt_editorpane_new(NULL, NULL, NULL, NULL);
  const char *initial =
      "---\n"
      "title: \"Incremental Stress\"\n"
      "links: [\"[[welcome]]\"]\n"
      "---\n\n"
      "First paragraph with [[welcome]] and https://example.org/one.\n\n"
      "Second paragraph [note](projects/conpty.md) and [[02-bootstrap|Boot]].\n\n"
      "```c\n[[in_fence]]\n```\n\n"
      "Third paragraph $[[in_math]]$ and `[[in_code]]` and [[chapter-01/01-intro.md]].\n\n"
      "Final paragraph [[tail_note]].\n";

  bt_editorpane_load(pane, "stress.md", initial, strlen(initial));
  assert_spans_match_full(pane, "initial");

  GtkTextView *tv = GTK_TEXT_VIEW(bt_editorpane_view_widget(pane));
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);

  GRand *rng = g_rand_new_with_seed(0x4C323031u);
  const char *snippets[] = {
    " [[alpha]] ",
    " [doc](sub/doc.md) ",
    " https://example.org/p(1) ",
    "\n\n",
    "\n",
    " word ",
    "`[[code]]`",
    "$[[m]]$",
    "```\n[[f]]\n```",
    "<!-- [[c]] -->",
    " \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E ",
  };

  for (int step = 0; step < 500; step++) {
    gint nchars = gtk_text_buffer_get_char_count(buf);
    gboolean do_delete = (nchars > 60) && (g_rand_int_range(rng, 0, 3) == 0);
    gchar label[48];
    g_snprintf(label, sizeof(label), "step_%d", step);

    if (do_delete) {
      gint p0 = g_rand_int_range(rng, 0, nchars - 1);
      gint del_len = g_rand_int_range(rng, 1, MIN(24, nchars - p0) + 1);
      GtkTextIter s, e;
      gtk_text_buffer_get_iter_at_offset(buf, &s, p0);
      gtk_text_buffer_get_iter_at_offset(buf, &e, p0 + del_len);
      gtk_text_buffer_delete(buf, &s, &e);
    } else {
      gint pos = (nchars > 0) ? g_rand_int_range(rng, 0, nchars + 1) : 0;
      const char *sn = snippets[g_rand_int_range(rng, 0, (gint32)G_N_ELEMENTS(snippets))];
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_offset(buf, &it, pos);
      gtk_text_buffer_insert(buf, &it, sn, -1);
    }
    assert_spans_match_full(pane, label);
  }

  g_rand_free(rng);
  printf("BT_TEST_L2_1 incremental_edits=500 final_spans=%u\n",
         bt_editorpane_link_spans(pane)->len);
  bt_editorpane_free(pane);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_l2_1_incremental_500_seeded_edits),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
