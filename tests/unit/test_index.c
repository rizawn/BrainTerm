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

static void test_migration_v1_to_v2(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-idx-mig-XXXXXX", NULL);
  gchar *v1_path = g_build_filename(root, "v1_seed.db", NULL);

  /* Create a genuine v1 database using the untouched migrations/0001_init.sql semantics. */
  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(v1_path, &db), SQLITE_OK);
  const char *v1_sql =
      "CREATE TABLE notes("
      "  id TEXT PRIMARY KEY, path TEXT UNIQUE NOT NULL, title TEXT NOT NULL DEFAULT '',"
      "  body TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, updated INTEGER NOT NULL,"
      "  source TEXT NOT NULL CHECK(source IN ('human','ai','import')), agent TEXT,"
      "  fm_hash TEXT NOT NULL, body_hash TEXT NOT NULL, parse_error INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE tags(note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
      "  tag TEXT NOT NULL, origin TEXT NOT NULL CHECK(origin IN ('human','ai')), confidence REAL,"
      "  PRIMARY KEY(note_id, tag, origin));"
      "CREATE TABLE edges(src TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
      "  dst TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('link','similar','temporal','tagged')),"
      "  weight REAL NOT NULL DEFAULT 1.0, PRIMARY KEY(src, dst, kind));"
      "CREATE TABLE sessions(id TEXT PRIMARY KEY, tool TEXT NOT NULL, cwd TEXT NOT NULL,"
      "  started INTEGER NOT NULL, ended INTEGER, resume_cmd TEXT,"
      "  note_id TEXT REFERENCES notes(id) ON DELETE SET NULL);"
      "CREATE TABLE embeddings(note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
      "  chunk INTEGER NOT NULL, vec BLOB NOT NULL, PRIMARY KEY(note_id, chunk));"
      "CREATE VIRTUAL TABLE notes_fts USING fts5(title, body, content='notes', content_rowid='rowid', tokenize='unicode61');"
      "CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN INSERT INTO notes_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body); END;"
      "CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN INSERT INTO notes_fts(notes_fts, rowid, title, body) VALUES('delete', old.rowid, old.title, old.body); END;"
      "CREATE TRIGGER notes_au AFTER UPDATE ON notes BEGIN INSERT INTO notes_fts(notes_fts, rowid, title, body) VALUES('delete', old.rowid, old.title, old.body); INSERT INTO notes_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body); END;"
      "INSERT INTO notes(id, path, title, body, created, updated, source, agent, fm_hash, body_hash, parse_error)"
      "  VALUES('bt-legacy', 'legacy.md', 'Legacy Note', 'legacy body', 100, 100, 'human', NULL, 'aa', 'bb', 0);"
      "PRAGMA user_version=1;";
  assert_int_equal(sqlite3_exec(db, v1_sql, NULL, NULL, NULL), SQLITE_OK);
  sqlite3_close(db);

  /* Copy v1 DB into a fresh vault's .bt/index.db and migrate v1 -> v2. */
  gchar *bt_dir = g_build_filename(root, ".bt", NULL);
  g_mkdir_with_parents(bt_dir, 0755);
  gchar *dst_db = g_build_filename(bt_dir, "index.db", NULL);
  gchar *bytes = NULL;
  gsize blen = 0;
  assert_true(g_file_get_contents(v1_path, &bytes, &blen, NULL));
  assert_true(g_file_set_contents(dst_db, bytes, (gssize)blen, NULL));
  g_free(bytes);
  g_free(dst_db);
  g_free(bt_dir);
  g_free(v1_path);

  bt_test_put(root, "source.md",
              "---\nid: bt-src\ntitle: \"Source\"\ncreated: 2026-11-23T00:00:00Z\nsource: human\n---\n"
              "Link [[target]] and [[ghost_note]].\n");
  BtVault *v = vault_open(root, NULL, NULL);
  gboolean r2 = FALSE;
  BtIndex *idx = bt_index_open(root, &r2, NULL);
  assert_non_null(idx);
  assert_false(r2);
  assert_int_equal(bt_index_user_version(idx), 2);

  /* Existing v1 row must survive migration. */
  GPtrArray *notes = bt_index_list_notes(idx, NULL);
  assert_int_equal(notes->len, 1);
  assert_string_equal(((BtNoteRow *)g_ptr_array_index(notes, 0))->id, "bt-legacy");
  g_ptr_array_unref(notes);

  assert_true(bt_index_upsert_note(idx, v, "source.md", NULL, NULL));
  GPtrArray *lr1 = bt_index_list_link_resolutions(idx, "bt-src", NULL);
  assert_int_equal(lr1->len, 2);
  for (guint i = 0; i < lr1->len; i++) {
    const BtLinkResRow *r = g_ptr_array_index(lr1, i);
    assert_string_equal(r->status, "unresolved");
  }
  g_ptr_array_unref(lr1);

  /* Create target.md -> existing unresolved link must automatically re-resolve to 'resolved'. */
  bt_test_put(root, "target.md", "# Target\n");
  assert_true(vault_rescan(v, NULL, NULL));
  assert_true(bt_index_upsert_note(idx, v, "target.md", NULL, NULL));
  GPtrArray *lr2 = bt_index_list_link_resolutions(idx, "bt-src", NULL);
  assert_int_equal(lr2->len, 2);
  gboolean found_resolved = FALSE, found_ghost = FALSE;
  for (guint i = 0; i < lr2->len; i++) {
    const BtLinkResRow *r = g_ptr_array_index(lr2, i);
    if (strcmp(r->raw_target, "target") == 0) {
      assert_string_equal(r->status, "resolved");
      assert_string_equal(r->resolved_path, "target.md");
      found_resolved = TRUE;
    } else if (strcmp(r->raw_target, "ghost_note") == 0) {
      assert_string_equal(r->status, "unresolved");
      found_ghost = TRUE;
    }
  }
  assert_true(found_resolved && found_ghost);
  g_ptr_array_unref(lr2);

  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_schema_fts_and_golden_rebuild),
    cmocka_unit_test(test_r2_corrupt_and_higher_version_refused),
    cmocka_unit_test(test_migration_v1_to_v2),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
