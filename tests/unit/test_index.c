/* test_index.c — M4 schema, FTS5 trigger sync, P5 golden rebuild, R2 corruption
 * auto-rebuild, higher user_version refusal, and sessions.json import. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <sqlite3.h>
#include <string.h>

#include "index.h"
#include "sessions.h"
#include "testutil.h"
#include "vault.h"

static void test_schema_fts_and_golden_rebuild(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-idx-XXXXXX", NULL);
  bt_test_put(root, "a.md",
              "---\nid: bt-1\ntitle: \"Alpha\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\ntags: [t1]\n---\n"
              "UniqueKeywordOne [[b]]\n");
  bt_test_put(root, "b.md",
              "---\nid: bt-1\ntitle: \"Beta\"\ncreated: 2026-11-22T00:00:00Z\nsource: ai\ntags_ai: [t2]\n---\n"
              "UniqueKeywordTwo\n"); /* intentional id collision -> bt-1-2 */

  BtVault *v = vault_open(root, NULL, NULL);
  gboolean r2 = FALSE;
  BtIndex *idx = bt_index_open(root, &r2, NULL);
  assert_non_null(idx);
  assert_false(r2);
  assert_int_equal(bt_index_user_version(idx), BT_SCHEMA_VERSION);

  /* Golden rebuild #1 and #2 must be byte-identical (P5). */
  assert_true(bt_index_rebuild(idx, v, NULL));
  assert_int_equal(bt_index_note_count(idx), 2);
  gchar *dump1 = bt_index_dump_canonical(idx, NULL);

  assert_true(bt_index_rebuild(idx, v, NULL));
  gchar *dump2 = bt_index_dump_canonical(idx, NULL);
  assert_string_equal(dump1, dump2);
  assert_non_null(strstr(dump1, "N|bt-1|a.md|"));
  assert_non_null(strstr(dump1, "N|bt-1-2|b.md|"));
  g_free(dump1);
  g_free(dump2);

  /* FTS5 trigger sync: insert -> match; update -> old gone, new present; delete -> gone. */
  assert_int_equal(bt_index_count_fts_matches(idx, "UniqueKeywordOne"), 1);

  bt_test_put(root, "a.md",
              "---\nid: bt-1\ntitle: \"Alpha\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\n---\n"
              "UpdatedTokenXYZ\n");
  gboolean skipped = TRUE;
  assert_true(bt_index_upsert_note(idx, v, "a.md", &skipped, NULL));
  assert_false(skipped);
  assert_int_equal(bt_index_count_fts_matches(idx, "UniqueKeywordOne"), 0);
  assert_int_equal(bt_index_count_fts_matches(idx, "UpdatedTokenXYZ"), 1);

  /* Second upsert with unchanged file must no-op via hash equality. */
  assert_true(bt_index_upsert_note(idx, v, "a.md", &skipped, NULL));
  assert_true(skipped);

  assert_true(bt_index_delete_path(idx, "a.md", NULL));
  assert_int_equal(bt_index_count_fts_matches(idx, "UpdatedTokenXYZ"), 0);

  /* Import M3 sessions.json. */
  BtSessionStore *ss = bt_sessions_open(root, NULL);
  bt_sessions_add(ss, "claude-code", root, "claude --resume x\r", "b.md", NULL, NULL);
  bt_sessions_free(ss);
  assert_true(bt_index_import_sessions_json(idx, root, NULL));

  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_r2_corrupt_and_higher_version_refused(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-idx-r2-XXXXXX", NULL);
  bt_test_put(root, ".bt/index.db", "not a valid sqlite database at all!!!");

  gboolean r2 = FALSE;
  BtIndex *idx = bt_index_open(root, &r2, NULL);
  assert_non_null(idx);
  assert_true(r2); /* R2 detected corruption and rebuilt cleanly */
  bt_index_close(idx);

  /* Bump user_version to 99 -> must refuse downgrade! */
  gchar *dbp = g_build_filename(root, ".bt", "index.db", NULL);
  sqlite3 *raw = NULL;
  assert_int_equal(sqlite3_open(dbp, &raw), SQLITE_OK);
  assert_int_equal(sqlite3_exec(raw, "PRAGMA user_version=99;", NULL, NULL, NULL), SQLITE_OK);
  sqlite3_close(raw);
  g_free(dbp);

  GError *err = NULL;
  assert_null(bt_index_open(root, NULL, &err));
  assert_non_null(err);
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_INVALID));
  g_error_free(err);

  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_schema_fts_and_golden_rebuild),
    cmocka_unit_test(test_r2_corrupt_and_higher_version_refused),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
