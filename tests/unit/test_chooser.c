/* test_chooser.c — Headless chooser model tests CH01..CH11 (Phase L3-2a). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "choosermodel.h"
#include "linkresolve.h"
#include "testutil.h"
#include "vault.h"

/* CH01: Ranked list with 3 candidates preselects index 0. */
static void test_ch01_preselects_first_and_splits_paths(void **state)
{
  (void)state;
  const gchar *cands[] = {
    "projects/conpty.md",
    "projects/notes/conpty.md",
    "conpty.md",
    NULL,
  };
  BtChooserModel *m = bt_chooser_model_new("conpty", cands);
  assert_non_null(m);
  assert_int_equal(bt_chooser_model_count(m), 3);
  assert_int_equal(bt_chooser_model_selected(m), 0);
  assert_string_equal(bt_chooser_model_selected_relpath(m), "projects/conpty.md");

  g_autofree gchar *title = bt_chooser_model_title(m);
  assert_string_equal(title, "3 notes match \"conpty\"");

  const BtChooserItem *i0 = bt_chooser_model_item(m, 0);
  const BtChooserItem *i2 = bt_chooser_model_item(m, 2);
  assert_non_null(i0);
  assert_string_equal(i0->filename, "conpty.md");
  assert_string_equal(i0->folder, "projects");
  assert_non_null(i2);
  assert_string_equal(i2->filename, "conpty.md");
  assert_string_equal(i2->folder, "");

  bt_chooser_model_free(m);
}

/* CH02: Down from last row wraps to first. */
static void test_ch02_down_wraps_to_first(void **state)
{
  (void)state;
  const gchar *cands[] = {"a/x.md", "b/x.md", "c/x.md", NULL};
  BtChooserModel *m = bt_chooser_model_new("x", cands);
  assert_int_equal(bt_chooser_model_move_down(m), 1);
  assert_int_equal(bt_chooser_model_move_down(m), 2);
  assert_int_equal(bt_chooser_model_move_down(m), 0);
  assert_int_equal(bt_chooser_model_selected(m), 0);
  bt_chooser_model_free(m);
}

/* CH03: Up from first row wraps to last. */
static void test_ch03_up_wraps_to_last(void **state)
{
  (void)state;
  const gchar *cands[] = {"a/x.md", "b/x.md", "c/x.md", NULL};
  BtChooserModel *m = bt_chooser_model_new("x", cands);
  assert_int_equal(bt_chooser_model_selected(m), 0);
  assert_int_equal(bt_chooser_model_move_up(m), 2);
  assert_int_equal(bt_chooser_model_move_up(m), 1);
  bt_chooser_model_free(m);
}

/* CH04: Home and End select first and last candidates. */
static void test_ch04_home_and_end(void **state)
{
  (void)state;
  const gchar *cands[] = {"a/x.md", "b/x.md", "c/x.md", "d/x.md", NULL};
  BtChooserModel *m = bt_chooser_model_new("x", cands);
  assert_true(bt_chooser_model_select(m, 2));
  assert_int_equal(bt_chooser_model_end(m), 3);
  assert_string_equal(bt_chooser_model_selected_relpath(m), "d/x.md");
  assert_int_equal(bt_chooser_model_home(m), 0);
  assert_string_equal(bt_chooser_model_selected_relpath(m), "a/x.md");
  assert_false(bt_chooser_model_select(m, 99));
  assert_int_equal(bt_chooser_model_selected(m), 0);
  bt_chooser_model_free(m);
}

/* CH05: 14 candidates gives visible window 10 and footer "+4 more". */
static void test_ch05_visible_cap_10_and_footer_more(void **state)
{
  (void)state;
  const gchar *cands[15];
  gchar *storage[14];
  for (int i = 0; i < 14; i++) {
    storage[i] = g_strdup_printf("f%02d/dup.md", i);
    cands[i] = storage[i];
  }
  cands[14] = NULL;

  BtChooserModel *m = bt_chooser_model_new("dup", cands);
  assert_int_equal(bt_chooser_model_count(m), 14);
  assert_int_equal(bt_chooser_model_visible_count(m), 10);
  assert_int_equal(bt_chooser_model_overflow_count(m), 4);
  g_autofree gchar *footer = bt_chooser_model_footer_more(m);
  assert_string_equal(footer, "+4 more");

  bt_chooser_model_free(m);
  for (int i = 0; i < 14; i++)
    g_free(storage[i]);

  const gchar *small[] = {"a/x.md", "b/x.md", NULL};
  BtChooserModel *ms = bt_chooser_model_new("x", small);
  assert_int_equal(bt_chooser_model_visible_count(ms), 2);
  assert_int_equal(bt_chooser_model_overflow_count(ms), 0);
  assert_null(bt_chooser_model_footer_more(ms));
  bt_chooser_model_free(ms);
}

/* CH06: Copy full path link formats [[projects/conpty]] and exact status message. */
static void test_ch06_copy_full_path_link_formatting(void **state)
{
  (void)state;
  const gchar *cands[] = {
    "projects/conpty.md",
    "statistical-statistics-notes/MathStats.md",
    "notes/Guide.markdown",
    NULL,
  };
  BtChooserModel *m = bt_chooser_model_new("conpty", cands);

  g_autofree gchar *l0 = bt_chooser_model_copy_link(m, 0);
  g_autofree gchar *s0 = bt_chooser_model_copy_status(m, 0);
  assert_string_equal(l0, "[[projects/conpty]]");
  assert_string_equal(s0, "Copied link for conpty");

  g_autofree gchar *l1 = bt_chooser_model_copy_link(m, 1);
  g_autofree gchar *s1 = bt_chooser_model_copy_status(m, 1);
  assert_string_equal(l1, "[[statistical-statistics-notes/MathStats]]");
  assert_string_equal(s1, "Copied link for MathStats");

  g_autofree gchar *l2 = bt_chooser_model_copy_link(m, 2);
  g_autofree gchar *s2 = bt_chooser_model_copy_status(m, 2);
  assert_string_equal(l2, "[[notes/Guide]]");
  assert_string_equal(s2, "Copied link for Guide");

  bt_chooser_model_free(m);
}

/* CH07: Copy operation never modifies the source note file on disk. */
static void test_ch07_copy_never_modifies_source_file(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-ch07-XXXXXX", NULL);
  const gchar *src_body = "---\nid: bt-20261123-000101\ntitle: \"Src\"\n---\nUse [[dup]]\n";
  bt_test_put(root, "source.md", src_body);
  bt_test_put(root, "a/dup.md", "# A\n");
  bt_test_put(root, "b/dup.md", "# B\n");

  g_autofree gchar *sha_before = g_compute_checksum_for_string(G_CHECKSUM_SHA256, src_body, -1);

  BtVault *v = vault_open(root, NULL, NULL);
  assert_non_null(v);
  BtIndexSnapshot *snap = bt_index_snapshot_from_vault(v);
  BtLinkResolution *res = bt_link_resolve(snap, "source.md", "dup");
  assert_int_equal(res->status, BT_LINK_AMBIGUOUS);

  BtChooserModel *m = bt_chooser_model_new("dup", (const gchar *const *)res->candidates);
  g_autofree gchar *copied = bt_chooser_model_copy_link(m, 1);
  assert_non_null(copied);
  assert_true(g_str_has_prefix(copied, "[["));

  gchar *disk = NULL;
  gsize dlen = 0;
  assert_true(vault_read(v, "source.md", &disk, &dlen, NULL));
  g_autofree gchar *sha_after = g_compute_checksum_for_string(G_CHECKSUM_SHA256, disk, (gssize)dlen);
  assert_string_equal(sha_before, sha_after);

  g_free(disk);
  bt_chooser_model_free(m);
  bt_link_resolution_free(res);
  bt_index_snapshot_free(snap);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

/* CH08: Candidate deleted after snapshot reports "That note no longer exists". */
static void test_ch08_candidate_deleted_after_snapshot(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-ch08-XXXXXX", NULL);
  bt_test_put(root, "a/dup.md", "# A\n");
  bt_test_put(root, "b/dup.md", "# B\n");

  BtVault *v = vault_open(root, NULL, NULL);
  assert_non_null(v);
  BtIndexSnapshot *snap = bt_index_snapshot_from_vault(v);
  BtLinkResolution *res = bt_link_resolve(snap, "root.md", "dup");
  assert_int_equal(res->status, BT_LINK_AMBIGUOUS);

  BtChooserModel *m = bt_chooser_model_new("dup", (const gchar *const *)res->candidates);
  assert_true(bt_chooser_candidate_exists(v, bt_chooser_model_selected_relpath(m), NULL));

  /* Delete selected candidate externally on disk while snapshot & model stay open. */
  g_autofree gchar *victim = g_build_filename(root, bt_chooser_model_selected_relpath(m), NULL);
  assert_int_equal(g_remove(victim), 0);

  g_autofree gchar *msg = NULL;
  assert_false(bt_chooser_candidate_exists(v, bt_chooser_model_selected_relpath(m), &msg));
  assert_string_equal(msg, "That note no longer exists");

  bt_chooser_model_free(m);
  bt_link_resolution_free(res);
  bt_index_snapshot_free(snap);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

/* CH09: Zero candidates at open time returns NONE_MATCH. */
static void test_ch09_zero_candidates_decides_none(void **state)
{
  (void)state;
  const gchar *empty[] = {NULL};
  assert_int_equal(bt_chooser_decide_open(NULL), BT_CHOOSER_OPEN_NONE_MATCH);
  assert_int_equal(bt_chooser_decide_open(empty), BT_CHOOSER_OPEN_NONE_MATCH);
}

/* CH10: One candidate after re-resolve opens directly (DIRECT). */
static void test_ch10_one_candidate_decides_direct(void **state)
{
  (void)state;
  const gchar *one[] = {"a/dup.md", NULL};
  const gchar *two[] = {"a/dup.md", "b/dup.md", NULL};
  assert_int_equal(bt_chooser_decide_open(one), BT_CHOOSER_OPEN_DIRECT);
  assert_int_equal(bt_chooser_decide_open(two), BT_CHOOSER_OPEN_SHOW);
}

/* CH11: Watcher / snapshot mutation while model is open does not mutate the model list. */
static void test_ch11_model_list_immutable_on_external_change(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-ch11-XXXXXX", NULL);
  bt_test_put(root, "a/dup.md", "# A\n");
  bt_test_put(root, "b/dup.md", "# B\n");

  BtVault *v = vault_open(root, NULL, NULL);
  BtIndexSnapshot *snap1 = bt_index_snapshot_from_vault(v);
  BtLinkResolution *res1 = bt_link_resolve(snap1, "src.md", "dup");
  assert_int_equal(res1->status, BT_LINK_AMBIGUOUS);

  BtChooserModel *m = bt_chooser_model_new("dup", (const gchar *const *)res1->candidates);
  bt_link_resolution_free(res1);
  assert_int_equal(bt_chooser_model_count(m), 2);

  /* Simulated watcher event: a third duplicate arrives and snapshot rebuilds. */
  bt_test_put(root, "c/dup.md", "# C\n");
  vault_rescan(v, NULL, NULL);
  BtIndexSnapshot *snap2 = bt_index_snapshot_from_vault(v);
  BtLinkResolution *res2 = bt_link_resolve(snap2, "src.md", "dup");
  assert_int_equal(res2->status, BT_LINK_AMBIGUOUS);
  guint new_cnt = 0;
  while (res2->candidates[new_cnt] != NULL)
    new_cnt++;
  assert_int_equal(new_cnt, 3);

  /* Open model stays at 2 items until next open. */
  assert_int_equal(bt_chooser_model_count(m), 2);
  assert_string_equal(bt_chooser_model_selected_relpath(m), "a/dup.md");

  bt_chooser_model_free(m);
  bt_link_resolution_free(res2);
  bt_index_snapshot_free(snap1);
  bt_index_snapshot_free(snap2);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_ch01_preselects_first_and_splits_paths),
    cmocka_unit_test(test_ch02_down_wraps_to_first),
    cmocka_unit_test(test_ch03_up_wraps_to_last),
    cmocka_unit_test(test_ch04_home_and_end),
    cmocka_unit_test(test_ch05_visible_cap_10_and_footer_more),
    cmocka_unit_test(test_ch06_copy_full_path_link_formatting),
    cmocka_unit_test(test_ch07_copy_never_modifies_source_file),
    cmocka_unit_test(test_ch08_candidate_deleted_after_snapshot),
    cmocka_unit_test(test_ch09_zero_candidates_decides_none),
    cmocka_unit_test(test_ch10_one_candidate_decides_direct),
    cmocka_unit_test(test_ch11_model_list_immutable_on_external_change),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
