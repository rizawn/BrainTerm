/* test_checkpoint.c — ADR-006 / P7 / R10 libgit2 checkpoint tests. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <glib/gstdio.h>
#include <string.h>

#include "checkpoint.h"
#include "testutil.h"

static void test_init_take_modify_rollback(void **state)
{
  (void)state;
  bt_checkpoint_init();
  gchar *root = g_dir_make_tmp("bt-cp-XXXXXX", NULL);
  assert_non_null(root);

  /* Empty vault init + checkpoint works. */
  gchar oid0[BT_OID_HEX_LEN] = {0};
  assert_true(bt_checkpoint_take(root, "bt-init empty", oid0, NULL));
  assert_int_equal(strlen(oid0), 40);

  /* Add note, take checkpoint 1, modify note, rollback -> original restored. */
  bt_test_put(root, "note.md", "original content\n");
  gchar oid1[BT_OID_HEX_LEN] = {0};
  assert_true(bt_checkpoint_take(root, "bt-launch claude-code", oid1, NULL));
  assert_string_not_equal(oid0, oid1);

  bt_test_put(root, "note.md", "corrupted by rogue agent\n");
  assert_true(bt_checkpoint_rollback(root, oid1, NULL));

  gchar *path = g_build_filename(root, "note.md", NULL);
  gchar *got = NULL;
  assert_true(g_file_get_contents(path, &got, NULL, NULL));
  assert_string_equal(got, "original content\n");
  g_free(got);
  g_free(path);

  /* List checkpoints returns newest first. */
  GPtrArray *list = bt_checkpoint_list(root, 10, NULL);
  assert_non_null(list);
  assert_int_equal(list->len, 2);
  BtCheckpointInfo *top = g_ptr_array_index(list, 0);
  assert_string_equal(top->oid, oid1);
  assert_string_equal(top->message, "bt-launch claude-code");
  g_ptr_array_unref(list);

  /* Read file from checkpoint blob. */
  gchar *blob = NULL;
  assert_true(bt_checkpoint_read_file(root, oid1, "note.md", &blob, NULL, NULL));
  assert_string_equal(blob, "original content\n");
  g_free(blob);

  bt_test_rm_rf(root);
  g_free(root);
  bt_checkpoint_shutdown();
}

static void test_locked_repo_refused_r10(void **state)
{
  (void)state;
  bt_checkpoint_init();
  gchar *root = g_dir_make_tmp("bt-cp-lock-XXXXXX", NULL);
  bt_test_put(root, "a.md", "hello\n");
  gchar oid[BT_OID_HEX_LEN] = {0};
  assert_true(bt_checkpoint_take(root, "initial", oid, NULL));

  /* Simulate locked .git/index.lock -> R10 must fail and set error. */
  bt_test_put(root, ".git/index.lock", "locked");
  GError *err = NULL;
  assert_false(bt_checkpoint_take(root, "should fail", oid, &err));
  assert_non_null(err);
  g_error_free(err);

  bt_test_rm_rf(root);
  g_free(root);
  bt_checkpoint_shutdown();
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_init_take_modify_rollback),
    cmocka_unit_test(test_locked_repo_refused_r10),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
