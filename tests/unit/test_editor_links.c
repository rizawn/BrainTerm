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

typedef struct {
  guint calls;
  gchar *last_target;
} ClickSpy;

static void on_spy_link(const gchar *target, gpointer ud)
{
  ClickSpy *s = ud;
  s->calls++;
  g_free(s->last_target);
  s->last_target = g_strdup(target);
}

static void test_l2_2_click_and_gesture_rules(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }

  ClickSpy spy = {0};
  BtEditorPane *pane = bt_editorpane_new(NULL, on_spy_link, NULL, &spy);
  const char *doc = "Intro text here. Click [[target-note]] or placeholder [[{{tpl}}]] end.\n";
  bt_editorpane_load(pane, "note.md", doc, strlen(doc));

  const GArray *spans = bt_editorpane_link_spans(pane);
  assert_int_equal(spans->len, 2);
  const BtLinkSpan *sp_ok = &g_array_index(spans, BtLinkSpan, 0);
  const BtLinkSpan *sp_tpl = &g_array_index(spans, BtLinkSpan, 1);
  gsize hit_off = sp_ok->target_start + 2;

  /* Caret starts at 0 (outside link). Single plain click opens and preserves caret at 0. */
  assert_true(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, 0));
  assert_int_equal(spy.calls, 1);
  assert_string_equal(spy.last_target, "target-note");
  assert_int_equal(bt_editorpane_caret_byte_offset(pane), 0);

  /* Placeholder link never opens on click. */
  assert_false(bt_editorpane_simulate_click(pane, sp_tpl->target_start + 1, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, 0));
  assert_int_equal(spy.calls, 1);

  /* Drag beyond threshold does not open. */
  bt_editorpane_set_caret_byte_offset(pane, 0);
  assert_false(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 1, 40.0, 0.0, 0));
  assert_int_equal(spy.calls, 1);

  /* Double-click and triple-click never open. */
  bt_editorpane_set_caret_byte_offset(pane, 0);
  assert_false(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 2, 0.0, 0.0, 0));
  bt_editorpane_set_caret_byte_offset(pane, 0);
  assert_false(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 3, 0.0, 0.0, 0));
  assert_int_equal(spy.calls, 1);

  /* Active selection blocks click. */
  GtkTextView *tv = GTK_TEXT_VIEW(bt_editorpane_view_widget(pane));
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
  GtkTextIter s0, s1;
  gtk_text_buffer_get_iter_at_offset(buf, &s0, 0);
  gtk_text_buffer_get_iter_at_offset(buf, &s1, 5);
  gtk_text_buffer_select_range(buf, &s0, &s1);
  assert_false(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, 0));
  gtk_text_buffer_place_cursor(buf, &s0);
  assert_int_equal(spy.calls, 1);

  /* Alt+Click places caret inside link without opening. */
  assert_false(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, GDK_ALT_MASK));
  assert_int_equal(spy.calls, 1);
  assert_int_equal(bt_editorpane_caret_byte_offset(pane), hit_off);

  /* Caret-inside rule: clicking inside link when caret is already inside does not open. */
  assert_false(bt_editorpane_simulate_click(pane, hit_off + 2, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, 0));
  assert_int_equal(spy.calls, 1);

  /* Mode ctrl_click: plain click outside fails, Ctrl+Click opens, Middle-click opens. */
  bt_editorpane_set_caret_byte_offset(pane, 0);
  bt_editorpane_set_links_open_on_click(pane, FALSE);
  assert_false(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, 0));
  assert_int_equal(spy.calls, 1);
  bt_editorpane_set_caret_byte_offset(pane, 0);
  assert_true(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_PRIMARY, 1, 0.0, 0.0, GDK_CONTROL_MASK));
  assert_int_equal(spy.calls, 2);
  assert_int_equal(bt_editorpane_caret_byte_offset(pane), 0);
  assert_true(bt_editorpane_simulate_click(pane, hit_off, GDK_BUTTON_MIDDLE, 1, 0.0, 0.0, 0));
  assert_int_equal(spy.calls, 3);

  printf("BT_TEST_L2_2 click_rules_passed=1 spy_calls=%u\n", spy.calls);
  g_free(spy.last_target);
  bt_editorpane_free(pane);
}

static void test_l2_3_hover_styles_and_tooltips(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }

  BtIndexSnapshot *snap = bt_index_snapshot_new();
  bt_index_snapshot_add_entry(snap, "welcome.md", FALSE, "Welcome", NULL);
  bt_index_snapshot_add_entry(snap, "projects/conpty.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "projects/notes/conpty.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "assets/paper.pdf", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "scripts/run.ps1", FALSE, NULL, NULL);
  bt_index_snapshot_finalize(snap);

  BtEditorPane *pane = bt_editorpane_new(NULL, NULL, NULL, NULL);
  bt_editorpane_set_index_snapshot(pane, snap, NULL);

  const char *doc =
      "Ok [[welcome#Top]] file [[assets/paper.pdf]] ext https://user:secret@example.org/x\n"
      "Ambig [[conpty]] broken [[missing-note]] placeholder [[{{tpl}}]]\n"
      "Blocked [[scripts/run.ps1]]\n"
      "## AI draft\n"
      "Suggested [[welcome]] link\n";

  bt_editorpane_load(pane, "welcome.md", doc, strlen(doc));
  const GArray *spans = bt_editorpane_link_spans(pane);
  assert_int_equal(spans->len, 8);

  /* Check tag assignments (broken/blocked get distinct underline tags, placeholder gets none). */
  const BtLinkSpan *s_ok = &g_array_index(spans, BtLinkSpan, 0);
  const BtLinkSpan *s_file = &g_array_index(spans, BtLinkSpan, 1);
  const BtLinkSpan *s_url = &g_array_index(spans, BtLinkSpan, 2);
  const BtLinkSpan *s_ambig = &g_array_index(spans, BtLinkSpan, 3);
  const BtLinkSpan *s_broken = &g_array_index(spans, BtLinkSpan, 4);
  const BtLinkSpan *s_tpl = &g_array_index(spans, BtLinkSpan, 5);
  const BtLinkSpan *s_blocked = &g_array_index(spans, BtLinkSpan, 6);
  const BtLinkSpan *s_ai = &g_array_index(spans, BtLinkSpan, 7);

  assert_true(bt_editorpane_offset_has_tag_name(pane, s_ok->target_start, "bt-wikilink"));
  assert_true(bt_editorpane_offset_has_tag_name(pane, s_broken->target_start, "bt-link-broken"));
  assert_true(bt_editorpane_offset_has_tag_name(pane, s_blocked->target_start, "bt-link-blocked"));
  assert_false(bt_editorpane_offset_has_tag_name(pane, s_tpl->target_start, "bt-wikilink"));
  assert_false(bt_editorpane_offset_has_tag_name(pane, s_tpl->target_start, "bt-link-broken"));
  assert_false(bt_editorpane_offset_has_tag_name(pane, s_tpl->target_start, "bt-syntax"));

  /* Tooltips per status + userinfo warning + AI draft suffix. */
  g_autofree gchar *t0 = bt_editorpane_tooltip_for_offset(pane, s_ok->start);
  g_autofree gchar *t1 = bt_editorpane_tooltip_for_offset(pane, s_file->start);
  g_autofree gchar *t2 = bt_editorpane_tooltip_for_offset(pane, s_url->start);
  g_autofree gchar *t3 = bt_editorpane_tooltip_for_offset(pane, s_ambig->start);
  g_autofree gchar *t4 = bt_editorpane_tooltip_for_offset(pane, s_broken->start);
  g_autofree gchar *t5 = bt_editorpane_tooltip_for_offset(pane, s_tpl->start);
  g_autofree gchar *t6 = bt_editorpane_tooltip_for_offset(pane, s_blocked->start);
  g_autofree gchar *t7 = bt_editorpane_tooltip_for_offset(pane, s_ai->start);

  assert_string_equal(t0, "welcome.md#Top");
  assert_string_equal(t1, "assets/paper.pdf — Opens in default app");
  assert_non_null(strstr(t2, "https://example.org/x"));
  assert_null(strstr(t2, "secret"));
  assert_non_null(strstr(t2, "Warning"));
  assert_string_equal(t3, "2 notes match. Click to choose.");
  assert_string_equal(t4, "Note not found");
  assert_null(t5);
  assert_non_null(strstr(t6, "Blocked"));
  assert_string_equal(t7, "welcome.md · AI draft");

  /* Cursor rule in default vs ctrl_click mode. */
  assert_string_equal(bt_editorpane_cursor_name_at_offset(pane, s_ok->start, 0), "pointer");
  assert_string_equal(bt_editorpane_cursor_name_at_offset(pane, s_tpl->start, 0), "text");
  bt_editorpane_set_links_open_on_click(pane, FALSE);
  assert_string_equal(bt_editorpane_cursor_name_at_offset(pane, s_ok->start, 0), "text");
  assert_string_equal(bt_editorpane_cursor_name_at_offset(pane, s_ok->start, GDK_CONTROL_MASK), "pointer");

  printf("BT_TEST_L2_3 tooltips_and_cursors_passed=1\n");
  bt_editorpane_set_index_snapshot(pane, NULL, NULL);
  bt_editorpane_free(pane);
  bt_index_snapshot_free(snap);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_l2_1_incremental_500_seeded_edits),
    cmocka_unit_test(test_l2_2_click_and_gesture_rules),
    cmocka_unit_test(test_l2_3_hover_styles_and_tooltips),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
