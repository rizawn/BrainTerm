/* test_crashrig.c — R1 (§9): a process killed at every stage of an atomic
 * write leaves the target as the complete old or complete new version, and
 * the next vault open removes the residue. The child path comes from the
 * BT_CRASHRIG_CHILD environment variable (set by meson). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "testutil.h"
#include "vault.h"

#define CRASHRIG_LINES 300000u /* must match crashrig_child.c */

static const gchar *const stage_names[] = {
  "tmp-opened", "tmp-partial", "tmp-synced", "before-rename", "after-rename", "no-crash",
};

static void run_stage(int stage)
{
  const gchar *child = g_getenv("BT_CRASHRIG_CHILD");
  assert_non_null(child);
  gchar *root = g_dir_make_tmp("bt-crash-XXXXXX", NULL);
  assert_non_null(root);

  gsize old_len = 0, new_len = 0;
  gchar *old_data = bt_test_payload("OLD", CRASHRIG_LINES / 2, &old_len);
  gchar *new_data = bt_test_payload("NEW", CRASHRIG_LINES, &new_len);
  bt_test_put(root, "note.md", old_data);

  gchar *stage_s = g_strdup_printf("%d", stage);
  gchar *argv[] = {(gchar *)child, root, "note.md", stage_s, NULL};
  gint wait_status = 0;
  GError *err = NULL;
  assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL,
                           &wait_status, &err));
  gboolean clean_exit = g_spawn_check_wait_status(wait_status, NULL);
  if (stage == BT_WRITE_STAGE_COUNT)
    assert_true(clean_exit);
  else
    assert_false(clean_exit); /* it really died mid-write */

  gchar *got = NULL;
  gsize got_len = 0;
  gchar *path = g_build_filename(root, "note.md", NULL);
  assert_true(g_file_get_contents(path, &got, &got_len, NULL));
  gboolean is_old = got_len == old_len && memcmp(got, old_data, old_len) == 0;
  gboolean is_new = got_len == new_len && memcmp(got, new_data, new_len) == 0;
  if (!is_old && !is_new)
    fail_msg("stage %s: note is a MIXTURE (len %" G_GSIZE_FORMAT ")", stage_names[stage], got_len);
  /* Before the rename the old version must survive; from the rename on, new. */
  if (stage < BT_WRITE_STAGE_AFTER_RENAME)
    assert_true(is_old);
  else
    assert_true(is_new);

  /* Reopen: residue removed, never surfaced as a note. */
  BtScanStats st;
  BtVault *v = vault_open(root, &st, NULL);
  assert_non_null(v);
  gchar *tmp = g_build_filename(root, "note.md.tmp", NULL);
  assert_false(g_file_test(tmp, G_FILE_TEST_EXISTS));
  assert_int_equal(vault_notes(v)->len, 1);
  assert_string_equal(g_ptr_array_index(vault_notes(v), 0), "note.md");
  if (stage < BT_WRITE_STAGE_BEFORE_RENAME + 1 && stage != BT_WRITE_STAGE_COUNT)
    assert_int_equal(st.tmp_removed, 1);
  else
    assert_int_equal(st.tmp_removed, 0);
  vault_free(v);

  g_free(tmp);
  g_free(path);
  g_free(got);
  g_free(stage_s);
  g_free(old_data);
  g_free(new_data);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_tmp_opened(void **s) { (void)s; run_stage(BT_WRITE_STAGE_TMP_OPENED); }
static void test_tmp_partial(void **s) { (void)s; run_stage(BT_WRITE_STAGE_TMP_PARTIAL); }
static void test_tmp_synced(void **s) { (void)s; run_stage(BT_WRITE_STAGE_TMP_SYNCED); }
static void test_before_rename(void **s) { (void)s; run_stage(BT_WRITE_STAGE_BEFORE_RENAME); }
static void test_after_rename(void **s) { (void)s; run_stage(BT_WRITE_STAGE_AFTER_RENAME); }
static void test_no_crash(void **s) { (void)s; run_stage(BT_WRITE_STAGE_COUNT); }

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_tmp_opened),    cmocka_unit_test(test_tmp_partial),
    cmocka_unit_test(test_tmp_synced),    cmocka_unit_test(test_before_rename),
    cmocka_unit_test(test_after_rename),  cmocka_unit_test(test_no_crash),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
