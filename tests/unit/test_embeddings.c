/* test_embeddings.c — M8 optional offline embeddings, cosine similarity edges,
 * disableable on/off round-trip, and hybrid vs FTS search. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "embed.h"
#include "index.h"
#include "testutil.h"
#include "vault.h"

static void test_embeddings_on_off_and_hybrid(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-emb-XXXXXX", NULL);
  bt_test_put(root, "a.md", "# Terminal Emulator\nConPTY pseudo-terminal scrollback rendering\n");
  bt_test_put(root, "b.md", "# Terminal Grid\nConPTY pseudo-terminal scrollback rendering fast\n");
  bt_test_put(root, "c.md", "# Cooking Pasta\nBoil water add salt and basil sauce\n");

  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));

  /* Cosine sanity: a & b very similar, a & c unrelated. */
  gfloat va[BT_EMBED_DIM], vb[BT_EMBED_DIM], vc[BT_EMBED_DIM];
  bt_embed_text("ConPTY pseudo-terminal scrollback rendering", va);
  bt_embed_text("ConPTY pseudo-terminal scrollback rendering fast", vb);
  bt_embed_text("Boil water add salt and basil sauce", vc);
  assert_true(bt_embed_cosine(va, vb) > BT_SIMILAR_THRESHOLD);
  assert_true(bt_embed_cosine(va, vc) < 0.4);

  /* Enable embeddings -> similar edge created. */
  guint sim_edges = 0;
  assert_true(bt_embed_sync_vault(v, idx, TRUE, &sim_edges, NULL));
  assert_true(sim_edges >= 1);

  /* Hybrid search finds morphological/trigram match ("crollback") that
   * prefix-only FTS5 genuinely misses! */
  GPtrArray *fts_only = bt_embed_hybrid_search(v, idx, "crollback", 10, FALSE, NULL);
  GPtrArray *hybrid = bt_embed_hybrid_search(v, idx, "crollback", 10, TRUE, NULL);
  assert_int_equal(fts_only->len, 0);
  assert_true(hybrid->len >= 1);
  g_ptr_array_unref(fts_only);
  g_ptr_array_unref(hybrid);

  /* Disable embeddings -> cleanly clears vectors, FTS unaffected. */
  assert_true(bt_embed_sync_vault(v, idx, FALSE, &sim_edges, NULL));
  assert_int_equal(sim_edges, 0);
  assert_int_equal(bt_index_count_fts_matches(idx, "ConPTY"), 2);

  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_embeddings_on_off_and_hybrid),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
