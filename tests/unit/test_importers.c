/* test_importers.c — M8 importer tests: conversations.json, .jsonl, hostile
 * traversal title, idempotency, original date preservation, P3 separation. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "frontmatter.h"
#include "importer.h"
#include "index.h"
#include "testutil.h"
#include "vault.h"

static void test_importers_and_idempotency(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-imp-XXXXXX", NULL);
  gchar *ext = g_dir_make_tmp("bt-imp-src-XXXXXX", NULL);
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);

  /* Fixture conversations.json with a normal chat AND a hostile traversal title. */
  const gchar *conv_json =
      "[\n"
      "  {\"title\": \"ConPTY Design\", \"create_time\": 1700000000, \"messages\": [\n"
      "    {\"role\": \"user\", \"content\": \"How does ConPTY work?\"},\n"
      "    {\"role\": \"assistant\", \"content\": \"ConPTY uses CreatePseudoConsole.\"}\n"
      "  ]},\n"
      "  {\"title\": \"../../etc/passwd\", \"create_time\": 1700000000, \"messages\": [\n"
      "    {\"role\": \"user\", \"content\": \"tricky\"}\n"
      "  ]}\n"
      "]\n";
  bt_test_put(ext, "conversations.json", conv_json);
  gchar *jp = g_build_filename(ext, "conversations.json", NULL);

  BtImportStats st;
  assert_true(bt_import_conversations_json(v, idx, "chatgpt", jp, &st, NULL));
  assert_int_equal(st.imported, 2); /* traversal title safely slugified inside inbox/import/chatgpt/ */
  assert_int_equal(st.rejected, 0);

  /* Re-import must be idempotent (0 imported, 2 skipped). */
  assert_true(bt_import_conversations_json(v, idx, "chatgpt", jp, &st, NULL));
  assert_int_equal(st.imported, 0);
  assert_int_equal(st.skipped, 2);

  /* Verify P3 separation + original date preserved (2023-11-14). */
  gchar *note = NULL;
  assert_true(vault_read(v, "inbox/import/chatgpt/2023-11-14-conpty-design.md", &note, NULL, NULL));
  assert_non_null(strstr(note, "source: import"));
  assert_non_null(strstr(note, "created: 2023-11-14"));
  assert_non_null(strstr(note, "## AI draft"));
  assert_non_null(strstr(note, "CreatePseudoConsole"));
  g_free(note);
  g_free(jp);

  /* Claude Code .jsonl import */
  const gchar *jsonl =
      "{\"timestamp\":\"2026-11-20T10:00:00Z\",\"role\":\"user\",\"content\":\"Fix PTY\"}\n"
      "{\"timestamp\":\"2026-11-20T10:01:00Z\",\"role\":\"assistant\",\"content\":\"Fixed!\"}\n";
  bt_test_put(ext, "sess1.jsonl", jsonl);
  gchar *lp = g_build_filename(ext, "sess1.jsonl", NULL);
  assert_true(bt_import_session_jsonl(v, idx, "claude-code", lp, &st, NULL));
  assert_int_equal(st.imported, 1);
  g_free(lp);

  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(ext);
  bt_test_rm_rf(root);
  g_free(ext);
  g_free(root);
}

static void test_importer_yaml_title_escaping(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-imp-esc-XXXXXX", NULL);
  gchar *ext = g_dir_make_tmp("bt-imp-esrc-XXXXXX", NULL);
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);

  const gchar *special_title = "Quote \"A\" \\ Slash\ninjected: true";
  const gchar *conv_json =
      "[\n"
      "  {\"title\": \"Quote \\\"A\\\" \\\\ Slash\\ninjected: true\", \"create_time\": 1700000000, \"messages\": [\n"
      "    {\"role\": \"user\", \"content\": \"Test YAML escaping\"}\n"
      "  ]}\n"
      "]\n";
  bt_test_put(ext, "escape.json", conv_json);
  gchar *jp = g_build_filename(ext, "escape.json", NULL);

  BtImportStats st;
  assert_true(bt_import_conversations_json(v, idx, "chatgpt", jp, &st, NULL));
  assert_int_equal(st.imported, 1);
  assert_true(vault_rescan(v, NULL, NULL));

  const GPtrArray *notes = vault_notes(v);
  assert_int_equal(notes->len, 1);
  const gchar *rel = g_ptr_array_index(notes, 0);

  gchar *raw = NULL;
  gsize len = 0;
  assert_true(vault_read(v, rel, &raw, &len, NULL));
  BtFrontmatter *fm = bt_frontmatter_parse(raw, len);
  assert_false(fm->parse_error);
  assert_string_equal(fm->title, special_title);
  assert_string_equal(fm->source, "import");

  bt_frontmatter_free(fm);
  g_free(raw);
  g_free(jp);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(ext);
  bt_test_rm_rf(root);
  g_free(ext);
  g_free(root);
}

static void test_importer_nonascii_slug_and_malformed_jsonl(void **state)
{
  (void)state;
  gchar *s_empty = bt_importer_slug("");
  assert_string_equal(s_empty, "untitled");
  g_free(s_empty);

  gchar *s_jp = bt_importer_slug("日本語テスト");
  gchar *s_ar = bt_importer_slug("تجربة");
  assert_true(g_str_has_prefix(s_jp, "untitled-"));
  assert_true(g_str_has_prefix(s_ar, "untitled-"));
  assert_string_not_equal(s_jp, s_ar);
  g_free(s_jp);
  g_free(s_ar);

  gchar *root = g_dir_make_tmp("bt-imp-bad-XXXXXX", NULL);
  gchar *ext = g_dir_make_tmp("bt-imp-bsrc-XXXXXX", NULL);
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);

  bt_test_put(ext, "bad.jsonl", "not json at all\n{broken json\n");
  gchar *bp = g_build_filename(ext, "bad.jsonl", NULL);
  BtImportStats st;
  GError *err = NULL;
  assert_false(bt_import_session_jsonl(v, idx, "codex", bp, &st, &err));
  assert_non_null(err);
  g_clear_error(&err);
  assert_true(vault_rescan(v, NULL, NULL));
  assert_int_equal(vault_notes(v)->len, 0);

  g_free(bp);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(ext);
  bt_test_rm_rf(root);
  g_free(ext);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_importers_and_idempotency),
    cmocka_unit_test(test_importer_yaml_title_escaping),
    cmocka_unit_test(test_importer_nonascii_slug_and_malformed_jsonl),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
