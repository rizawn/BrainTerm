/* bench_search.c — §10 budget: FTS5 query over 1,000 notes < 100 ms, plus
 * ask-bar mode detection & command autocomplete verification. */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "index.h"
#include "search.h"
#include "testutil.h"
#include "vault.h"

#define BT_FTS_BUDGET_MS 100.0

int main(void)
{
  /* Quick sanity check on ask-bar mode detection & command autocomplete. */
  const gchar *q = NULL;
  if (bt_search_detect_mode(">rebuild", &q) != BT_ASK_MODE_COMMAND || strcmp(q, "rebuild") != 0)
    return 10;
  if (bt_search_detect_mode("?what is ConPTY", &q) != BT_ASK_MODE_ASK)
    return 11;
  if (bt_search_detect_mode("how does ConPTY work?", &q) != BT_ASK_MODE_ASK)
    return 12;
  if (bt_search_detect_mode("conpty", &q) != BT_ASK_MODE_SEARCH)
    return 13;
  GPtrArray *cmds = bt_search_match_commands("rebuild");
  if (cmds->len == 0)
    return 14;
  g_ptr_array_unref(cmds);

  gchar *root = g_dir_make_tmp("bt-fts-bench-XXXXXX", NULL);
  for (guint i = 0; i < 1000; i++) {
    gchar *rel = g_strdup_printf("dir-%02u/note-%04u.md", i % 20, i);
    gchar *body = g_strdup_printf(
        "---\nid: bt-20261122-%06u\ntitle: \"Note %u\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\n---\n"
        "# Note %u\n\nThis note discusses %s architecture and local-first storage.\n",
        i, i, i, (i % 50 == 0) ? "ConPTY pseudo-terminal" : "markdown indexing");
    bt_test_put(root, rel, body);
    g_free(body);
    g_free(rel);
  }

  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  if (!bt_index_rebuild(idx, v, NULL))
    return 2;

  gint64 t0 = g_get_monotonic_time();
  GPtrArray *hits = bt_index_search_fts(idx, "ConPTY", 50, NULL);
  double ms = (double)(g_get_monotonic_time() - t0) / 1000.0;

  printf("BT_BENCH fts_1k notes=%u hits=%u elapsed_ms=%.2f budget_ms=%.0f\n",
         bt_index_note_count(idx), hits ? hits->len : 0, ms, BT_FTS_BUDGET_MS);

  gchar *ask_cmd = bt_search_compose_ask_agent_cmd("what is ConPTY?", hits, 3);
  int rc = (hits != NULL && hits->len == 20 && ms < BT_FTS_BUDGET_MS &&
            strstr(ask_cmd, "claude \"Answer using [[") != NULL)
               ? 0
               : 1;
  g_free(ask_cmd);
  g_ptr_array_unref(hits);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
  return rc;
}
