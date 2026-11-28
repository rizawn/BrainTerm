/* test_mcp.c — ADR-008 standalone read-only bt-mcp stdio JSON-RPC tests. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <cjson/cJSON.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "index.h"
#include "testutil.h"
#include "vault.h"

static void test_mcp_stdio_roundtrip(void **state)
{
  (void)state;
  const gchar *bin = g_getenv("BT_MCP_BIN");
  assert_non_null(bin);

  gchar *root = g_dir_make_tmp("bt-mcp-XXXXXX", NULL);
  bt_test_put(root, "note.md", "---\nid: bt-m1\ntitle: \"MCP Memory\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\n---\nBrainTerm local memory bridge.\n");
  bt_test_put(root, "linker.md", "---\nid: bt-m2\ntitle: \"Linker\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\n---\nSee [[note]].\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));
  bt_index_close(idx);
  vault_free(v);

  /* Regression check: bt-mcp must open vault strictly read-only and preserve .tmp files. */
  bt_test_put(root, "note.md.tmp", "in-flight atomic write");

  const gchar *req =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"search_memory\",\"arguments\":{\"query\":\"BrainTerm\",\"top_k\":5}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"ask_memory\",\"arguments\":{\"query\":\"memory\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"get_note\",\"arguments\":{\"path\":\"note.md\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"backlinks\",\"arguments\":{\"path\":\"note.md\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"delete_note\",\"arguments\":{\"path\":\"note.md\"}}}\n";

  GSubprocess *proc = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
      NULL, bin, "--vault", root, NULL);
  assert_non_null(proc);

  gchar *out = NULL, *err = NULL;
  assert_true(g_subprocess_communicate_utf8(proc, req, NULL, &out, &err, NULL));
  g_object_unref(proc);

  gchar **lines = g_strsplit(out, "\n", -1);
  assert_non_null(lines[0]);
  assert_non_null(strstr(lines[0], "\"readOnly\":true"));
  assert_non_null(strstr(lines[0], "\"capabilities\""));
  assert_non_null(strstr(lines[1], "search_memory"));
  assert_non_null(strstr(lines[1], "\"inputSchema\""));
  const char *ask_pos = strstr(lines[1], "\"ask_memory\"");
  const char *topk_pos = strstr(lines[1], "\"top_k\"");
  assert_true(topk_pos != NULL && ask_pos != NULL && topk_pos < ask_pos);
  assert_non_null(strstr(lines[2], "[[MCP Memory]]"));
  assert_non_null(strstr(lines[3], "\"mode\":\"evidence\""));
  assert_non_null(strstr(lines[4], "BrainTerm local memory bridge."));
  assert_non_null(strstr(lines[5], "linker.md"));
  assert_non_null(strstr(lines[6], "\"error\"")); /* write tool refused! */

  gchar *tmp_path = g_build_filename(root, "note.md.tmp", NULL);
  assert_true(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
  g_free(tmp_path);

  g_strfreev(lines);
  g_free(out);
  g_free(err);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_mcp_large_line_over_64kb(void **state)
{
  (void)state;
  const gchar *bin = g_getenv("BT_MCP_BIN");
  assert_non_null(bin);

  gchar *root = g_dir_make_tmp("bt-mcp-big-XXXXXX", NULL);
  bt_test_put(root, "huge.md", "---\nid: bt-mb1\ntitle: \"Huge\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\n---\nBig line OK.\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));
  bt_index_close(idx);
  vault_free(v);

  /* Build a >70 KB single JSON-RPC request line (beyond the legacy 64 KB fgets buffer). */
  GString *pad = g_string_new(NULL);
  for (int i = 0; i < 7200; i++)
    g_string_append(pad, "aaaaaaaaa ");
  g_autofree gchar *req = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
      "\"params\":{\"name\":\"search_memory\",\"arguments\":{\"query\":\"%s\"}}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
      "\"params\":{\"name\":\"get_note\",\"arguments\":{\"path\":\"huge.md\"}}}\n",
      pad->str);
  g_string_free(pad, TRUE);
  assert_true(strlen(req) > 70000);

  GSubprocess *proc = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
      NULL, bin, "--vault", root, NULL);
  assert_non_null(proc);

  gchar *out = NULL, *err = NULL;
  assert_true(g_subprocess_communicate_utf8(proc, req, NULL, &out, &err, NULL));
  g_object_unref(proc);

  gchar **lines = g_strsplit(out, "\n", -1);
  assert_non_null(lines[0]);
  assert_non_null(strstr(lines[0], "\"id\":1"));
  assert_non_null(lines[1]);
  assert_non_null(strstr(lines[1], "Big line OK."));

  g_strfreev(lines);
  g_free(out);
  g_free(err);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_mcp_stdio_roundtrip),
    cmocka_unit_test(test_mcp_large_line_over_64kb),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
