/* test_importers.c — M8 importer tests: conversations.json, .jsonl, hostile
 * traversal title, idempotency, original date preservation, P3 separation. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_importers_and_idempotency),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
