/* test_editor_links.c — Part A Phase L2 editor link model, gestures, tooltips & nav tests. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <adwaita.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "createnote.h"
#include "linkresolve.h"
#include "navhistory.h"
#include "testutil.h"
#include "ui/chooserpopover.h"
#include "ui/createpopover.h"
#include "ui/editorpane.h"
#include "vault.h"

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

static void test_l2_5_flush_before_navigation(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();

  gchar *root = g_dir_make_tmp("bt-l25-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg25-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);
  bt_test_put(root, "a.md", "# A\nOriginal text.\n");
  bt_test_put(root, "b.md", "# B\nSecond note.\n");

  BtApp *app = bt_app_new();
  bt_app_prepare_ui_for_test(app);
  assert_true(bt_app_open_vault(app, root, NULL));
  assert_true(bt_app_open_note(app, "a.md", NULL));
  assert_string_equal(bt_app_active_note(app), "a.md");

  BtEditorPane *ed = bt_app_editor(app);
  GtkTextView *tv = GTK_TEXT_VIEW(bt_editorpane_view_widget(ed));
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
  GtkTextIter end_it;
  gtk_text_buffer_get_end_iter(buf, &end_it);
  gtk_text_buffer_insert(buf, &end_it, "Unsaved line.\n", -1);
  assert_true(bt_editorpane_is_dirty(ed));

  /* Inject write failure -> navigation to b.md MUST abort and keep a.md open & dirty. */
  bt_app_set_fail_next_save_for_test(app, TRUE);
  GError *nav_err = NULL;
  assert_false(bt_app_open_note(app, "b.md", &nav_err));
  assert_non_null(nav_err);
  g_clear_error(&nav_err);
  assert_string_equal(bt_app_active_note(app), "a.md");
  assert_true(bt_editorpane_is_dirty(ed));
  assert_non_null(strstr(bt_app_last_statusbar_notice(app), "Save failed"));

  /* Now allow write -> navigating to b.md flushes a.md atomically to disk first. */
  assert_true(bt_app_open_note(app, "b.md", NULL));
  assert_string_equal(bt_app_active_note(app), "b.md");
  g_autofree gchar *disk_a = NULL;
  g_autofree gchar *path_a = g_build_filename(root, "a.md", NULL);
  assert_true(g_file_get_contents(path_a, &disk_a, NULL, NULL));
  assert_non_null(strstr(disk_a, "Unsaved line."));

  printf("BT_TEST_L2_5 flush_blocks_on_error=1 flushed_on_success=1\n");
  GtkWindow *win = bt_app_window(app);
  if (win) gtk_window_destroy(win);
  bt_app_free(app);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

typedef struct {
  guint calls;
  gchar *last_target;
  gboolean last_is_uri;
} LauncherMock;

static gboolean on_test_launcher(const char *target_or_url, gboolean is_uri, void *ud)
{
  LauncherMock *m = ud;
  m->calls++;
  g_free(m->last_target);
  m->last_target = g_strdup(target_or_url);
  m->last_is_uri = is_uri;
  return TRUE;
}

static void test_l2_4_dispatcher_and_launcher_mock(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();

  gchar *root = g_dir_make_tmp("bt-l24-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg24-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);
  bt_test_put(root, "a.md", "# A\nLinks to [[b#Core Concepts]] and [[scripts/run.ps1]].\n");
  bt_test_put(root, "b.md",
              "# B\nline 1\nline 2\nline 3\nline 4\nline 5\n"
              "## Core Concepts\nBody under Core Concepts.\n");
  bt_test_put(root, "assets/paper.pdf", "%PDF-1.4\n");
  bt_test_put(root, "assets/data.xyz", "custom binary");
  bt_test_put(root, "scripts/run.ps1", "Write-Host 1\n");

  BtApp *app = bt_app_new();
  bt_app_prepare_ui_for_test(app);
  LauncherMock mock = {0};
  bt_app_set_launcher_for_test(app, on_test_launcher, &mock);

  assert_true(bt_app_open_vault(app, root, NULL));
  assert_true(bt_app_open_note(app, "a.md", NULL));

  /* 1. Note link with anchor -> opens b.md in same pane, scrolls to heading, 0 external calls. */
  assert_true(bt_app_activate_link(app, "b#Core Concepts"));
  assert_string_equal(bt_app_active_note(app), "b.md");
  assert_int_equal(mock.calls, 0);

  /* 2. SAME_NOTE anchor scroll. */
  assert_true(bt_app_activate_link(app, "#Core Concepts"));
  assert_string_equal(bt_app_active_note(app), "b.md");
  assert_int_equal(mock.calls, 0);

  /* 3. Blocked cases: zero calls, no confirm popover, status-bar reason. */
  const char *blocked_targets[] = {
    "scripts/run.ps1",
    "javascript:alert(1)",
    "../../escape",
    "file:///C:/Windows/calc.exe",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(blocked_targets); i++) {
    assert_false(bt_app_activate_link(app, blocked_targets[i]));
    assert_int_equal(mock.calls, 0);
    assert_null(bt_app_active_confirm_popover_for_test(app));
    assert_non_null(strstr(bt_app_last_statusbar_notice(app), "Blocked"));
  }

  /* 4. Unconfirmed external URL (with userinfo): 0 launcher calls until confirmed, strips secret. */
  assert_true(bt_app_activate_link(app, "https://user:secret@example.org/doc"));
  assert_int_equal(mock.calls, 0);
  assert_non_null(bt_app_active_confirm_popover_for_test(app));
  assert_non_null(strstr(bt_app_active_confirm_detail_for_test(app), "https://example.org/doc"));
  assert_null(strstr(bt_app_active_confirm_detail_for_test(app), "secret"));
  assert_true(bt_app_confirm_active_popover_for_test(app, TRUE)); /* Don't ask again */
  assert_int_equal(mock.calls, 1);
  assert_true(mock.last_is_uri);

  /* Subsequent external URL in same session skips confirm popover. */
  assert_true(bt_app_activate_link(app, "https://example.org/two"));
  assert_int_equal(mock.calls, 2);
  assert_null(bt_app_active_confirm_popover_for_test(app));

  /* 5. Non-allow-listed extension -> confirm popover first, then launches after confirm. */
  assert_true(bt_app_activate_link(app, "assets/data.xyz"));
  assert_int_equal(mock.calls, 2);
  assert_non_null(bt_app_active_confirm_popover_for_test(app));
  assert_true(bt_app_confirm_active_popover_for_test(app, FALSE));
  assert_int_equal(mock.calls, 3);
  assert_false(mock.last_is_uri);

  /* 6. Directory -> confirm popover first, then reveals after confirm. */
  assert_true(bt_app_activate_link(app, "assets"));
  assert_int_equal(mock.calls, 3);
  assert_non_null(bt_app_active_confirm_popover_for_test(app));
  assert_true(bt_app_confirm_active_popover_for_test(app, FALSE));
  assert_int_equal(mock.calls, 4);

  /* 7. Allow-listed file (.pdf) -> launches immediately without confirm. */
  assert_true(bt_app_activate_link(app, "assets/paper.pdf"));
  assert_int_equal(mock.calls, 5);
  assert_null(bt_app_active_confirm_popover_for_test(app));

  /* 8. Placeholder -> no-op, 0 calls. */
  assert_true(bt_app_activate_link(app, "{{template}}"));
  assert_int_equal(mock.calls, 5);

  printf("BT_TEST_L2_4 dispatcher_mock_calls=%u\n", mock.calls);
  g_free(mock.last_target);
  GtkWindow *win = bt_app_window(app);
  if (win) gtk_window_destroy(win);
  bt_app_free(app);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

static guint count_files_recursive(const gchar *dir_path)
{
  guint total = 0;
  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (dir == NULL)
    return 0;
  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    g_autofree gchar *full = g_build_filename(dir_path, name, NULL);
    if (g_file_test(full, G_FILE_TEST_IS_DIR))
      total += count_files_recursive(full);
    else
      total++;
  }
  g_dir_close(dir);
  return total;
}

static void test_l2_8_unresolved_click_does_not_call_stub_writer(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();
  bt_app_reset_stub_writer_calls_for_test();

  gchar *root = g_dir_make_tmp("bt-l28-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg28-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);
  const gchar *note_text = "# A\nUnresolved target [[missing-note]] here.\n";
  bt_test_put(root, "a.md", note_text);

  BtApp *app = bt_app_new();
  bt_app_prepare_ui_for_test(app);
  assert_true(bt_app_open_vault(app, root, NULL));
  assert_true(bt_app_open_note(app, "a.md", NULL));

  guint files_before = count_files_recursive(root);
  BtEditorPane *ed = (BtEditorPane *)bt_app_editor(app);
  assert_non_null(ed);

  /* Place caret outside the link, then click inside [[missing-note]] via real editorpane click path. */
  const gchar *hit = strstr(note_text, "missing-note");
  assert_non_null(hit);
  gsize target_off = (gsize)(hit - note_text);
  bt_editorpane_set_caret_byte_offset(ed, 0);
  bt_app_set_statusbar_notice(app, NULL);

  assert_true(bt_editorpane_simulate_click(ed, target_off, 1, 1, 0.0, 0.0, 0));
  assert_string_equal(bt_app_active_note(app), "a.md");
  BtCreatePopover *cp = (BtCreatePopover *)bt_app_create_popover_for_test(app);
  assert_non_null(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_SHOWN);
  assert_string_equal(bt_createpopover_path_text(cp), "missing-note.md");
  assert_null(bt_app_active_confirm_popover_for_test(app));
  assert_int_equal(count_files_recursive(root), files_before);
  bt_createpopover_activate_cancel(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);

  /* Also verify nested path target via bt_app_activate_link opens popover and writes 0 files. */
  bt_app_set_statusbar_notice(app, NULL);
  assert_false(bt_app_activate_link(app, "folder/missing-two"));
  assert_string_equal(bt_app_active_note(app), "a.md");
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_SHOWN);
  assert_string_equal(bt_createpopover_path_text(cp), "folder/missing-two.md");
  assert_null(bt_app_active_confirm_popover_for_test(app));

  g_autofree gchar *p1 = g_build_filename(root, "missing-note.md", NULL);
  g_autofree gchar *p2 = g_build_filename(root, "folder", "missing-two.md", NULL);
  assert_false(g_file_test(p1, G_FILE_TEST_EXISTS));
  assert_false(g_file_test(p2, G_FILE_TEST_EXISTS));
  assert_int_equal(count_files_recursive(root), files_before);
  assert_int_equal(bt_app_stub_writer_calls_for_test(), 0);

  printf("BT_TEST_L2_8 unresolved_no_stub_create=1 files=%u\n", files_before);
  GtkWindow *win = bt_app_window(app);
  if (win) gtk_window_destroy(win);
  bt_app_free(app);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

static gchar *make_tall_note(const gchar *title, const gchar *next_link)
{
  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "# %s\nJump to %s\n## NearAnchor\nIntro paragraph.\n",
                         title, next_link ? next_link : "end");
  for (int i = 0; i < 80; i++)
    g_string_append_printf(s, "Line %03d padding text so scrolled window has vertical extent.\n", i);
  return g_string_free(s, FALSE);
}

static void test_l2_6_nav_history_5_jumps_and_deleted_skip(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();

  /* Part A: Pure unit check on BT_NAV_HISTORY_CAP (100). */
  BtNavHistory *cap = bt_nav_history_new();
  bt_nav_history_push_jump(cap, NULL, 0, 0.0, "n000.md", 0, 0.0);
  for (int i = 1; i <= 105; i++) {
    g_autofree gchar *prev = g_strdup_printf("n%03d.md", i - 1);
    g_autofree gchar *next = g_strdup_printf("n%03d.md", i);
    bt_nav_history_push_jump(cap, prev, (gsize)i, (gdouble)i * 10.0, next, 0, 0.0);
  }
  assert_int_equal((int)bt_nav_history_count(cap), BT_NAV_HISTORY_CAP);
  assert_int_equal(bt_nav_history_cursor(cap), BT_NAV_HISTORY_CAP - 1);
  assert_string_equal(bt_nav_history_at(cap, 0)->relpath, "n006.md");
  assert_string_equal(bt_nav_history_at(cap, BT_NAV_HISTORY_CAP - 1)->relpath, "n105.md");
  bt_nav_history_free(cap);

  /* Part B: Real 5-jump navigation across 6 notes (n1..n6) + deleted note skip. */
  gchar *root = g_dir_make_tmp("bt-l26-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg26-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);

  const gchar *names[6] = {"n1.md", "n2.md", "n3.md", "n4.md", "n5.md", "n6.md"};
  const gchar *links[6] = {"[[n2]]", "[[n3]]", "[[n4]]", "[[n5]]", "[[n6]]", "[[#NearAnchor]]"};
  for (int i = 0; i < 6; i++) {
    g_autofree gchar *txt = make_tall_note(names[i], links[i]);
    bt_test_put(root, names[i], txt);
  }

  BtApp *app = bt_app_new();
  bt_app_prepare_ui_for_test(app);
  assert_true(bt_app_open_vault(app, root, NULL));
  assert_true(bt_app_open_note(app, "n1.md", NULL));

  BtEditorPane *ed = (BtEditorPane *)bt_app_editor(app);
  assert_non_null(ed);

  const gsize carets[6] = {18, 24, 30, 36, 42, 12};
  const gdouble scrolls[6] = {120.0, 210.0, 300.0, 390.0, 480.0, 50.0};

  bt_editorpane_set_caret_byte_offset(ed, carets[0]);
  bt_editorpane_set_scroll_y(ed, scrolls[0]);
  assert_true(bt_app_activate_link(app, "n2"));

  bt_editorpane_set_caret_byte_offset(ed, carets[1]);
  bt_editorpane_set_scroll_y(ed, scrolls[1]);
  assert_true(bt_app_open_note(app, "n3.md", NULL)); /* Sidebar / Ctrl+P style direct open */

  bt_editorpane_set_caret_byte_offset(ed, carets[2]);
  bt_editorpane_set_scroll_y(ed, scrolls[2]);
  assert_true(bt_app_activate_link(app, "n4"));

  bt_editorpane_set_caret_byte_offset(ed, carets[3]);
  bt_editorpane_set_scroll_y(ed, scrolls[3]);
  assert_true(bt_app_activate_link(app, "n5"));

  bt_editorpane_set_caret_byte_offset(ed, carets[4]);
  bt_editorpane_set_scroll_y(ed, scrolls[4]);
  assert_true(bt_app_activate_link(app, "n6"));

  bt_editorpane_set_caret_byte_offset(ed, carets[5]);
  bt_editorpane_set_scroll_y(ed, scrolls[5]);

  /* Short same-note scroll (< 1 viewport) must NOT push onto nav stack. */
  gsize hist_before = bt_nav_history_count(bt_app_nav_history(app));
  assert_int_equal((int)hist_before, 6);
  assert_true(bt_app_activate_link(app, "#NearAnchor"));
  assert_int_equal((int)bt_nav_history_count(bt_app_nav_history(app)), (int)hist_before);
  bt_editorpane_set_caret_byte_offset(ed, carets[5]);
  bt_editorpane_set_scroll_y(ed, scrolls[5]);

  /* 5 consecutive Back steps restore note + caret + scroll_y. */
  for (int i = 4; i >= 0; i--) {
    assert_true(bt_app_navigate_back(app));
    assert_string_equal(bt_app_active_note(app), names[i]);
    assert_int_equal((int)bt_editorpane_caret_byte_offset(ed), (int)carets[i]);
    assert_true(fabs(bt_editorpane_scroll_y(ed) - scrolls[i]) < 1.0);
  }
  assert_false(bt_app_navigate_back(app));
  assert_string_equal(bt_app_active_note(app), "n1.md");

  /* 5 consecutive Forward steps restore n2..n6 + caret + scroll_y. */
  for (int i = 1; i <= 5; i++) {
    assert_true(bt_app_navigate_forward(app));
    assert_string_equal(bt_app_active_note(app), names[i]);
    assert_int_equal((int)bt_editorpane_caret_byte_offset(ed), (int)carets[i]);
    assert_true(fabs(bt_editorpane_scroll_y(ed) - scrolls[i]) < 1.0);
  }
  assert_false(bt_app_navigate_forward(app));

  /* Delete n4.md from disk -> Back from n5 skips deleted n4 silently and lands on n3; Forward skips to n5. */
  assert_true(bt_app_navigate_back(app));
  assert_string_equal(bt_app_active_note(app), "n5.md");
  g_autofree gchar *dead_n4 = g_build_filename(root, "n4.md", NULL);
  assert_int_equal(g_remove(dead_n4), 0);

  assert_true(bt_app_navigate_back(app));
  assert_string_equal(bt_app_active_note(app), "n3.md");
  assert_int_equal((int)bt_editorpane_caret_byte_offset(ed), (int)carets[2]);
  assert_true(bt_app_navigate_forward(app));
  assert_string_equal(bt_app_active_note(app), "n5.md");
  assert_int_equal((int)bt_editorpane_caret_byte_offset(ed), (int)carets[4]);

  printf("BT_TEST_L2_6 nav_cap=%d back_forward_jumps=5 deleted_skip=1\n", BT_NAV_HISTORY_CAP);
  GtkWindow *win = bt_app_window(app);
  if (win) gtk_window_destroy(win);
  bt_app_free(app);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

static void test_l2_7_settings_persistence(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();

  gchar *root = g_dir_make_tmp("bt-l27-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg27-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);
  const gchar *a_txt = "# A\nGo to [[b]] now.\n";
  bt_test_put(root, "a.md", a_txt);
  bt_test_put(root, "b.md", "# B\nTarget note.\n");

  /* Default when state.ini does not exist yet is TRUE. */
  assert_true(bt_app_load_links_open_on_click());

  BtApp *app1 = bt_app_new();
  bt_app_prepare_ui_for_test(app1);
  assert_true(bt_app_open_vault(app1, root, NULL));
  assert_true(bt_app_open_note(app1, "a.md", NULL));

  BtEditorPane *ed1 = (BtEditorPane *)bt_app_editor(app1);
  assert_true(bt_app_get_links_open_on_click(app1));
  assert_true(bt_editorpane_get_links_open_on_click(ed1));

  /* Toggle menu action to FALSE -> persists [editor] links_open_on_click = false. */
  GActionMap *amap1 = G_ACTION_MAP(bt_app_application(app1));
  GAction *act1 = g_action_map_lookup_action(amap1, "links-single-click");
  assert_non_null(act1);
  g_action_change_state(act1, g_variant_new_boolean(FALSE));
  assert_false(bt_app_get_links_open_on_click(app1));
  assert_false(bt_editorpane_get_links_open_on_click(ed1));
  assert_false(bt_app_load_links_open_on_click());

  g_autofree gchar *ini_path = bt_app_state_file_path();
  g_autofree gchar *ini_raw = NULL;
  assert_true(g_file_get_contents(ini_path, &ini_raw, NULL, NULL));
  assert_non_null(strstr(ini_raw, "[editor]"));
  assert_non_null(strstr(ini_raw, "links_open_on_click=false"));

  /* With single-click disabled, bare click on [[b]] stays on a.md; Ctrl+Click opens b.md. */
  gsize off_b = (gsize)(strstr(a_txt, "[[b]]") + 2 - a_txt);
  bt_editorpane_set_caret_byte_offset(ed1, 0);
  assert_false(bt_editorpane_simulate_click(ed1, off_b, 1, 1, 0.0, 0.0, 0));
  assert_string_equal(bt_app_active_note(app1), "a.md");
  bt_editorpane_set_caret_byte_offset(ed1, 0);
  assert_true(bt_editorpane_simulate_click(ed1, off_b, 1, 1, 0.0, 0.0, GDK_CONTROL_MASK));
  assert_string_equal(bt_app_active_note(app1), "b.md");

  GtkWindow *w1 = bt_app_window(app1);
  if (w1) gtk_window_destroy(w1);
  bt_app_free(app1);

  /* Restart app with same config dir -> loads FALSE on startup. */
  BtApp *app2 = bt_app_new();
  bt_app_prepare_ui_for_test(app2);
  assert_true(bt_app_open_vault(app2, root, NULL));
  assert_true(bt_app_open_note(app2, "a.md", NULL));
  BtEditorPane *ed2 = (BtEditorPane *)bt_app_editor(app2);
  assert_false(bt_app_get_links_open_on_click(app2));
  assert_false(bt_editorpane_get_links_open_on_click(ed2));

  GAction *act2 = g_action_map_lookup_action(G_ACTION_MAP(bt_app_application(app2)),
                                             "links-single-click");
  assert_non_null(act2);
  GVariant *st2 = g_action_get_state(act2);
  assert_false(g_variant_get_boolean(st2));
  g_variant_unref(st2);

  bt_editorpane_set_caret_byte_offset(ed2, 0);
  assert_false(bt_editorpane_simulate_click(ed2, off_b, 1, 1, 0.0, 0.0, 0));
  assert_string_equal(bt_app_active_note(app2), "a.md");

  /* Re-enable single-click, persist, and confirm on 3rd app instance. */
  g_action_change_state(act2, g_variant_new_boolean(TRUE));
  assert_true(bt_app_load_links_open_on_click());
  GtkWindow *w2 = bt_app_window(app2);
  if (w2) gtk_window_destroy(w2);
  bt_app_free(app2);

  BtApp *app3 = bt_app_new();
  bt_app_prepare_ui_for_test(app3);
  assert_true(bt_app_open_vault(app3, root, NULL));
  assert_true(bt_app_open_note(app3, "a.md", NULL));
  BtEditorPane *ed3 = (BtEditorPane *)bt_app_editor(app3);
  assert_true(bt_app_get_links_open_on_click(app3));
  assert_true(bt_editorpane_get_links_open_on_click(ed3));
  bt_editorpane_set_caret_byte_offset(ed3, 0);
  assert_true(bt_editorpane_simulate_click(ed3, off_b, 1, 1, 0.0, 0.0, 0));
  assert_string_equal(bt_app_active_note(app3), "b.md");

  printf("BT_TEST_L2_7 settings_persisted=1 restart_verified=1\n");
  GtkWindow *w3 = bt_app_window(app3);
  if (w3) gtk_window_destroy(w3);
  bt_app_free(app3);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

typedef struct {
  guint confirm_count;
  guint cancel_count;
  gchar *last_path;
} CreatePopProbe;

static void on_cp_probe_confirm(const BtCreatePlan *plan, gpointer ud)
{
  CreatePopProbe *p = ud;
  p->confirm_count++;
  g_free(p->last_path);
  p->last_path = g_strdup(plan->target_relpath);
}

static void on_cp_probe_cancel(gpointer ud)
{
  CreatePopProbe *p = ud;
  p->cancel_count++;
}

static void test_l3_1d_create_popover_state_machine(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();

  gchar *root = g_dir_make_tmp("bt-l31d-XXXXXX", NULL);
  bt_test_put(root, "welcome.md", "# Welcome\n");
  bt_test_put(root, "projects/conpty.md", "# Existing\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndexSnapshot *snap = bt_index_snapshot_from_vault(v);

  GtkWidget *win = gtk_window_new();
  GtkWidget *anchor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), anchor);
  gtk_window_present(GTK_WINDOW(win));

  CreatePopProbe probe = {0};
  BtCreatePopover *cp = bt_createpopover_new(on_cp_probe_confirm, on_cp_probe_cancel, &probe);
  GdkRectangle rect = {10, 10, 8, 18};

  /* Non-OK planner results NEVER open popover. */
  const char *non_ok[] = { "welcome", "assets/paper.pdf", "CON", "../../escape", "{{tpl}}", "" };
  for (gsize i = 0; i < G_N_ELEMENTS(non_ok); i++) {
    BtCreatePlan *bp = bt_createnote_plan(v, snap, "welcome.md", non_ok[i]);
    assert_int_not_equal(bp->result, BT_CREATE_PLAN_OK);
    assert_false(bt_createpopover_show(cp, anchor, &rect, bp));
    assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);
    bt_createnote_plan_free(bp);
  }

  /* Show next-to-note PLAN_OK with Appendix A exact copy & warning when basename collides (CR12). */
  BtCreatePlan *p_same = bt_createnote_plan(v, snap, "welcome.md", "conpty");
  assert_int_equal(p_same->result, BT_CREATE_PLAN_OK);
  assert_true(bt_createpopover_show(cp, anchor, &rect, p_same));
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_SHOWN);
  assert_string_equal(bt_createpopover_title_text(cp), "Create \"conpty\"?");
  assert_string_equal(bt_createpopover_path_text(cp), "conpty.md");
  assert_string_equal(bt_createpopover_location_hint_text(cp), "Created next to this note");
  assert_string_equal(bt_createpopover_warning_text(cp), "Other notes named conpty exist");
  assert_string_equal(bt_createpopover_create_button_label(cp), "Create");

  /* CK14: Tab order in create popover: Create then Cancel, and Tab cycles cleanly. */
  GtkWidget *c_btn = bt_createpopover_create_button(cp);
  GtkWidget *x_btn = bt_createpopover_cancel_button(cp);
  assert_ptr_equal(gtk_widget_get_next_sibling(c_btn), x_btn);
  assert_ptr_equal(bt_createpopover_focused_button(cp), c_btn);
  assert_true(bt_createpopover_handle_key(cp, GDK_KEY_Tab, 0));
  assert_ptr_equal(bt_createpopover_focused_button(cp), x_btn);
  assert_true(bt_createpopover_handle_key(cp, GDK_KEY_Tab, 0));
  assert_ptr_equal(bt_createpopover_focused_button(cp), c_btn);

  /* Shown -> Esc closes and notifies cancel without writing files. */
  assert_true(bt_createpopover_handle_key(cp, GDK_KEY_Escape, 0));
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);
  assert_int_equal(probe.cancel_count, 1);
  assert_int_equal(probe.confirm_count, 0);
  bt_createnote_plan_free(p_same);

  /* Folder hint when target uses folder + Busy state protects against Esc & double-press (§14.1). */
  BtCreatePlan *p_dir = bt_createnote_plan(v, snap, "welcome.md", "brand/new/idea");
  assert_true(bt_createpopover_show(cp, anchor, &rect, p_dir));
  assert_string_equal(bt_createpopover_location_hint_text(cp), "Folder will be created if missing");
  assert_null(bt_createpopover_warning_text(cp));
  assert_true(bt_createpopover_handle_key(cp, GDK_KEY_Return, 0));
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_BUSY);
  assert_string_equal(bt_createpopover_create_button_label(cp), "Creating...");
  assert_false(gtk_widget_get_sensitive(c_btn));
  assert_int_equal(probe.confirm_count, 1);

  /* Esc and second Create press while Busy are ignored (§14.1, §6.1.8). */
  assert_true(bt_createpopover_handle_key(cp, GDK_KEY_Escape, 0));
  bt_createpopover_activate_create(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_BUSY);
  assert_int_equal(probe.confirm_count, 1);
  assert_int_equal(probe.cancel_count, 1);

  bt_createpopover_close(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);

  /* Ensure zero files written by popover UI itself. */
  g_autofree gchar *not_written = g_build_filename(root, "brand", "new", "idea.md", NULL);
  assert_false(g_file_test(not_written, G_FILE_TEST_EXISTS));

  bt_createnote_plan_free(p_dir);
  bt_createpopover_free(cp);
  gtk_window_destroy(GTK_WINDOW(win));
  bt_index_snapshot_free(snap);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(probe.last_path);
  g_free(root);
  printf("BT_TEST_L3_1D create_popover state_machine=ok ck14_tab_order=ok\n");
}

static void pump_until_create_closed(BtCreatePopover *cp)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (bt_createpopover_state(cp) != BT_CREATE_POPOVER_CLOSED &&
         g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
  }
  /* Drain any remaining idle ticks (graph refresh). */
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
}

static guint count_vault_user_files(const gchar *dir_path)
{
  guint total = 0;
  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (dir == NULL)
    return 0;
  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (g_strcmp0(name, ".bt") == 0 || g_strcmp0(name, ".git") == 0)
      continue;
    g_autofree gchar *full = g_build_filename(dir_path, name, NULL);
    if (g_file_test(full, G_FILE_TEST_IS_DIR))
      total += count_vault_user_files(full);
    else
      total++;
  }
  g_dir_close(dir);
  return total;
}

static void test_l3_1e_unresolved_click_create_flow(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();
  bt_app_reset_stub_writer_calls_for_test();

  gchar *root = g_dir_make_tmp("bt-l31e-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg31e-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);

  const char *src_body =
      "---\nid: bt-20261123-000010\ntitle: \"Source\"\ncreated: 2026-11-23T00:00:00Z\nsource: human\n---\n"
      "Click [[newly-minted]] or [[folder/deep-note]] or non-note [[missing.png]] or [[../../escape]].\n";
  bt_test_put(root, "source.md", src_body);
  g_autofree gchar *src_sha_before = g_compute_checksum_for_string(G_CHECKSUM_SHA256, src_body, -1);

  BtApp *app = bt_app_new();
  bt_app_prepare_ui_for_test(app);
  assert_true(bt_app_open_vault(app, root, NULL));
  assert_true(bt_app_open_note(app, "source.md", NULL));

  guint files_before = count_files_recursive(root);
  guint user_files_before = count_vault_user_files(root);
  BtEditorPane *ed = (BtEditorPane *)bt_app_editor(app);
  assert_non_null(ed);

  /* D-L3-05: Non-note target shows "Not in vault" tooltip and never opens create popover. */
  const char *off_png = strstr(src_body, "missing.png");
  assert_non_null(off_png);
  g_autofree gchar *tip_png = bt_editorpane_tooltip_for_offset(ed, (gsize)(off_png - src_body));
  assert_string_equal(tip_png, "Not in vault");
  const char *off_esc = strstr(src_body, "../../escape");
  g_autofree gchar *tip_esc = bt_editorpane_tooltip_for_offset(ed, (gsize)(off_esc - src_body));
  assert_string_equal(tip_esc, "Not in vault");

  bt_editorpane_set_caret_byte_offset(ed, 0);
  assert_true(bt_editorpane_simulate_click(ed, (gsize)(off_png - src_body), 1, 1, 0.0, 0.0, 0));
  assert_null(bt_app_create_popover_for_test(app));
  assert_non_null(strstr(bt_app_last_statusbar_notice(app), "Not a note"));
  assert_int_equal(count_files_recursive(root), files_before);

  /* CX08: Clicking UNRESOLVED note opens popover with 0 files created before Create is pressed. */
  const char *off_new = strstr(src_body, "newly-minted");
  assert_non_null(off_new);
  bt_editorpane_set_caret_byte_offset(ed, 0);
  assert_true(bt_editorpane_simulate_click(ed, (gsize)(off_new - src_body), 1, 1, 0.0, 0.0, 0));
  BtCreatePopover *cp = (BtCreatePopover *)bt_app_create_popover_for_test(app);
  assert_non_null(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_SHOWN);
  assert_string_equal(bt_createpopover_path_text(cp), "newly-minted.md");
  assert_int_equal(count_files_recursive(root), files_before);

  /* Cancel writes 0 files and keeps caret at 0. */
  bt_createpopover_activate_cancel(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);
  assert_int_equal(count_files_recursive(root), files_before);
  assert_int_equal(bt_editorpane_caret_byte_offset(ed), 0);

  /* Re-click and press Create -> async GTask creates note, updates index & styles, opens it, pushes nav. */
  assert_true(bt_editorpane_simulate_click(ed, (gsize)(off_new - src_body), 1, 1, 0.0, 0.0, 0));
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_SHOWN);
  bt_createpopover_activate_create(cp);
  pump_until_create_closed(cp);
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);

  assert_string_equal(bt_app_active_note(app), "newly-minted.md");
  assert_string_equal(bt_app_last_statusbar_notice(app), "Created newly-minted.md");
  assert_int_equal(count_vault_user_files(root), user_files_before + 1);
  g_autofree gchar *created_disk = g_build_filename(root, "newly-minted.md", NULL);
  assert_true(g_file_test(created_disk, G_FILE_TEST_IS_REGULAR));
  assert_int_equal(bt_app_stub_writer_calls_for_test(), 0);

  /* Source note file on disk is 100% unmodified. */
  g_autofree gchar *src_disk = NULL;
  gsize src_dlen = 0;
  assert_true(vault_read(bt_app_vault(app), "source.md", &src_disk, &src_dlen, NULL));
  g_autofree gchar *src_sha_after = g_compute_checksum_for_string(G_CHECKSUM_SHA256, src_disk, (gssize)src_dlen);
  assert_string_equal(src_sha_before, src_sha_after);

  /* CX03: Alt+Left navigates back to source.md, and [[newly-minted]] now has resolved style. */
  assert_true(bt_app_navigate_back(app));
  assert_string_equal(bt_app_active_note(app), "source.md");
  assert_true(bt_editorpane_offset_has_tag_name(ed, (gsize)(off_new - src_body), "bt-wikilink"));
  assert_false(bt_editorpane_offset_has_tag_name(ed, (gsize)(off_new - src_body), "bt-link-broken"));

  /* Second click on same link now opens directly with no create popover (§6.1.8). */
  bt_editorpane_set_caret_byte_offset(ed, 0);
  assert_true(bt_editorpane_simulate_click(ed, (gsize)(off_new - src_body), 1, 1, 0.0, 0.0, 0));
  assert_string_equal(bt_app_active_note(app), "newly-minted.md");
  assert_int_equal(bt_createpopover_state(cp), BT_CREATE_POPOVER_CLOSED);

  printf("BT_TEST_L3_1E cx03_nav_back=ok cx08_no_write_before_create=ok\n");
  GtkWindow *win = bt_app_window(app);
  if (win) gtk_window_destroy(win);
  bt_app_free(app);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

static void test_l3_2b_ambiguity_chooser_ui(void **state)
{
  (void)state;
  if (!gtk_init_check()) {
    skip();
    return;
  }
  adw_init();

  gchar *root = g_dir_make_tmp("bt-l32b-XXXXXX", NULL);
  gchar *cfg = g_dir_make_tmp("bt-cfg32b-XXXXXX", NULL);
  g_setenv("BT_CONFIG_DIR", cfg, TRUE);

  const char *src_body =
      "---\n"
      "id: bt-20261123-000200\n"
      "title: \"Chooser Source\"\n"
      "created: 2026-11-23T00:00:00Z\n"
      "source: human\n"
      "---\n"
      "Ambiguous [[conpty]] link here.\n";
  bt_test_put(root, "source.md", src_body);
  bt_test_put(root, "projects/conpty.md", "# Projects ConPTY\n");
  bt_test_put(root, "archive/conpty.md", "# Archive ConPTY\n");
  g_autofree gchar *src_sha_before = g_compute_checksum_for_string(G_CHECKSUM_SHA256, src_body, -1);

  BtApp *app = bt_app_new();
  bt_app_prepare_ui_for_test(app);
  assert_true(bt_app_open_vault(app, root, NULL));
  assert_true(bt_app_open_note(app, "source.md", NULL));

  BtEditorPane *ed = (BtEditorPane *)bt_app_editor(app);
  assert_non_null(ed);
  const char *off_amb = strstr(src_body, "conpty");
  assert_non_null(off_amb);
  gsize off = (gsize)(off_amb - src_body);

  /* Click ambiguous link -> chooser popover opens with 2 candidates, index 0 preselected. */
  bt_editorpane_set_caret_byte_offset(ed, 0);
  assert_true(bt_editorpane_simulate_click(ed, off, 1, 1, 0.0, 0.0, 0));
  BtChooserPopover *cp = (BtChooserPopover *)bt_app_chooser_popover_for_test(app);
  assert_non_null(cp);
  assert_true(bt_chooserpopover_is_open(cp));
  assert_int_equal(bt_chooserpopover_candidate_count(cp), 2);
  assert_int_equal(bt_chooserpopover_selected_index(cp), 0);
  assert_string_equal(bt_chooserpopover_title_text(cp), "2 notes match \"conpty\"");

  /* CK13: Escape closes chooser without navigating or changing source. */
  assert_true(bt_chooserpopover_handle_key(cp, GDK_KEY_Escape, 0));
  assert_false(bt_chooserpopover_is_open(cp));
  assert_string_equal(bt_app_active_note(app), "source.md");

  /* Re-open chooser, press Ctrl+C -> copies full path link & stays open! */
  assert_true(bt_editorpane_simulate_click(ed, off, 1, 1, 0.0, 0.0, 0));
  assert_true(bt_chooserpopover_is_open(cp));
  assert_true(bt_chooserpopover_handle_key(cp, GDK_KEY_Down, 0));
  assert_int_equal(bt_chooserpopover_selected_index(cp), 1);
  assert_true(bt_chooserpopover_handle_key(cp, GDK_KEY_c, GDK_CONTROL_MASK));
  assert_true(bt_chooserpopover_is_open(cp));
  assert_non_null(bt_app_last_copied_link_for_test(app));
  assert_true(strcmp(bt_app_last_copied_link_for_test(app), "[[archive/conpty]]") == 0 ||
              strcmp(bt_app_last_copied_link_for_test(app), "[[projects/conpty]]") == 0);
  assert_string_equal(bt_app_last_statusbar_notice(app), "Copied link for conpty");

  /* Record which note is candidate 2 (selected index 1), then press Enter -> opens candidate 2 (CX07). */
  g_autofree gchar *cand2_rel = NULL;
  {
    const gchar *copied = bt_app_last_copied_link_for_test(app);
    cand2_rel = (strcmp(copied, "[[archive/conpty]]") == 0)
                    ? g_strdup("archive/conpty.md")
                    : g_strdup("projects/conpty.md");
  }
  assert_true(bt_chooserpopover_handle_key(cp, GDK_KEY_Return, 0));
  assert_false(bt_chooserpopover_is_open(cp));
  assert_string_equal(bt_app_active_note(app), cand2_rel);

  /* Nav back to source.md, verify source unchanged on disk. */
  assert_true(bt_app_navigate_back(app));
  assert_string_equal(bt_app_active_note(app), "source.md");
  g_autofree gchar *disk_src = NULL;
  gsize dlen = 0;
  assert_true(vault_read(bt_app_vault(app), "source.md", &disk_src, &dlen, NULL));
  g_autofree gchar *src_sha_after = g_compute_checksum_for_string(G_CHECKSUM_SHA256, disk_src, (gssize)dlen);
  assert_string_equal(src_sha_before, src_sha_after);

  /* Re-open chooser, delete candidate 1 externally on disk while chooser is open, press Open -> status & refresh. */
  assert_true(bt_editorpane_simulate_click(ed, off, 1, 1, 0.0, 0.0, 0));
  assert_true(bt_chooserpopover_is_open(cp));
  assert_true(bt_chooserpopover_handle_key(cp, GDK_KEY_Down, 0));
  bt_chooserpopover_activate_copy(cp);
  const gchar *victim_rel = (strcmp(bt_app_last_copied_link_for_test(app), "[[archive/conpty]]") == 0)
                                ? "archive/conpty.md"
                                : "projects/conpty.md";
  g_autofree gchar *victim_abs = g_build_filename(root, victim_rel, NULL);
  assert_int_equal(g_remove(victim_abs), 0);
  bt_chooserpopover_activate_open(cp);
  assert_false(bt_chooserpopover_is_open(cp));
  assert_string_equal(bt_app_last_statusbar_notice(app), "That note no longer exists");
  assert_string_equal(bt_app_active_note(app), "source.md");

  /* Wait for idle refresh if queued. */
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);

  /* Now only 1 candidate remains -> clicking opens remaining note directly with no chooser (CH10). */
  bt_editorpane_set_caret_byte_offset(ed, 0);
  assert_true(bt_editorpane_simulate_click(ed, off, 1, 1, 0.0, 0.0, 0));
  assert_false(bt_chooserpopover_is_open(cp));
  assert_true(strcmp(bt_app_active_note(app), "source.md") != 0);

  printf("BT_TEST_L3_2B ck13_esc=ok cx07_cand2=ok copy_stays_open=ok missing_refresh=ok\n");
  GtkWindow *win = bt_app_window(app);
  if (win) gtk_window_destroy(win);
  bt_app_free(app);
  bt_test_rm_rf(root);
  bt_test_rm_rf(cfg);
  g_free(root);
  g_free(cfg);
}

static guint ui_log_warnings = 0;

static void on_ui_test_log(const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer ud)
{
  (void)level;
  (void)ud;
  ui_log_warnings++;
  g_printerr("Unexpected UI warning (%s): %s\n", domain ? domain : "", msg);
}

int main(void)
{
  g_log_set_handler("Gtk", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, on_ui_test_log, NULL);
  g_log_set_handler("Adwaita", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, on_ui_test_log, NULL);
  g_log_set_handler("GtkSourceView", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, on_ui_test_log, NULL);
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_l2_1_incremental_500_seeded_edits),
    cmocka_unit_test(test_l2_2_click_and_gesture_rules),
    cmocka_unit_test(test_l2_3_hover_styles_and_tooltips),
    cmocka_unit_test(test_l2_5_flush_before_navigation),
    cmocka_unit_test(test_l2_4_dispatcher_and_launcher_mock),
    cmocka_unit_test(test_l2_8_unresolved_click_does_not_call_stub_writer),
    cmocka_unit_test(test_l2_6_nav_history_5_jumps_and_deleted_skip),
    cmocka_unit_test(test_l2_7_settings_persistence),
    cmocka_unit_test(test_l3_1d_create_popover_state_machine),
    cmocka_unit_test(test_l3_1e_unresolved_click_create_flow),
    cmocka_unit_test(test_l3_2b_ambiguity_chooser_ui),
  };
  int rc = cmocka_run_group_tests(tests, NULL, NULL);
  if (ui_log_warnings > 0)
    return 1;
  return rc;
}
