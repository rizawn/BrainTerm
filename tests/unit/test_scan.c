/* test_scan.c — §6.A1 / §7.4 scan correctness: ignore list, residue cleanup,
 * deterministic order. Timing budgets live in tests/bench. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "testutil.h"
#include "vault.h"

static void test_scan_filters(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-scan-XXXXXX", NULL);
  assert_non_null(root);
  bt_test_put(root, "a.md", "a");
  bt_test_put(root, "B.MD", "b");
  bt_test_put(root, "notes.txt", "not a note");
  bt_test_put(root, "sub/c.md", "c");
  bt_test_put(root, "sub/deeper/d.md", "d");
  bt_test_put(root, "sub/half.md.tmp", "residue");
  bt_test_put(root, "top.md.tmp", "residue");
  static const gchar *const ignored[] = {
    ".git/x.md", ".obsidian/x.md", ".node_modules/x.md", "node_modules/x.md",
    "target/x.md", "dist/x.md", ".bt/x.md", "sub/dist/x.md", "drafts/secret.md", NULL,
  };
  for (guint i = 0; ignored[i] != NULL; i++)
    bt_test_put(root, ignored[i], "ignored");

  BtScanStats st;
  BtVault *v = vault_open(root, &st, NULL);
  assert_non_null(v);
  assert_int_equal(st.tmp_removed, 2);
  assert_int_equal(st.dirs_unreadable, 0);

  /* user pattern added after open applies on rescan */
  vault_add_ignore_pattern(v, "drafts");
  assert_true(vault_rescan(v, &st, NULL));

  const GPtrArray *notes = vault_notes(v);
  static const gchar *const want[] = {"B.MD", "a.md", "sub/c.md", "sub/deeper/d.md"};
  assert_int_equal(notes->len, G_N_ELEMENTS(want));
  assert_int_equal(st.notes, G_N_ELEMENTS(want));
  for (guint i = 0; i < G_N_ELEMENTS(want); i++)
    assert_string_equal(g_ptr_array_index(notes, i), want[i]);

  gchar *tmp1 = g_build_filename(root, "top.md.tmp", NULL);
  gchar *tmp2 = g_build_filename(root, "sub", "half.md.tmp", NULL);
  assert_false(g_file_test(tmp1, G_FILE_TEST_EXISTS));
  assert_false(g_file_test(tmp2, G_FILE_TEST_EXISTS));
  g_free(tmp1);
  g_free(tmp2);

  assert_true(vault_is_ignored_path(v, ".git/config.md"));
  assert_true(vault_is_ignored_path(v, "a/.bt/x.md"));
  assert_true(vault_is_ignored_path(v, "x.md.tmp"));
  assert_false(vault_is_ignored_path(v, "inbox/terminal/x.md"));

  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_open_errors(void **state)
{
  (void)state;
  GError *err = NULL;
  assert_null(vault_open("this/does/not/exist/hopefully", NULL, &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_NOT_FOUND)); /* R8 */
  g_clear_error(&err);
  assert_null(vault_open("", NULL, &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_INVALID));
  g_clear_error(&err);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_scan_filters),
    cmocka_unit_test(test_open_errors),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
