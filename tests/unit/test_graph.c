/* test_graph.c — M7 Brain Graph tests:
 *  - THE INVARIANT: node-count == file-count (incl. isolated, link-less, tag-less note)
 *  - Ghost nodes + broken traversal links (§8.3)
 *  - Deterministic layout + forced brainstem/spine
 *  - Filters (§6.C4) */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <math.h>
#include <string.h>

#include "graph.h"
#include "index.h"
#include "sessions.h"
#include "testutil.h"
#include "vault.h"

static void test_graph_invariant_ghosts_layout_and_filters(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-graph-XXXXXX", NULL);
  bt_test_put(root, "isolated.md", "# Completely Isolated Note\nNo links, no tags.\n");
  bt_test_put(root, "hub.md",
              "---\nid: bt-hub\ntitle: \"Hub\"\ncreated: 2026-11-22T00:00:00Z\nsource: human\ntags: [core]\n---\n"
              "Links to [[FutureNote]] and hostile [[../../escape]].\n");
  bt_test_put(root, "inbox/import/claude/chat1.md",
              "---\nid: bt-imp1\ntitle: \"Imported\"\ncreated: 2026-11-21T00:00:00Z\nsource: import\ntags_ai: [claude]\n---\n"
              "Back to [[hub]].\n");

  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));
  BtSessionStore *ss = bt_sessions_open(root, NULL);
  bt_sessions_add(ss, "claude-code", root, "claude --continue\r", "hub.md", NULL, NULL);

  BtGraph *g1 = bt_graph_build(v, idx, ss);
  BtGraph *g2 = bt_graph_build(v, idx, ss);

  /* THE INVARIANT: every .md file is exactly one note node! */
  assert_int_equal(g1->note_node_count, vault_notes(v)->len);
  assert_int_equal(g1->note_node_count, 3);
  assert_int_equal(g1->ghost_node_count, 2);

  /* Verify FutureNote ghost is clickable, ../../escape ghost is broken (§8.3). */
  gboolean saw_valid_ghost = FALSE, saw_broken_ghost = FALSE, saw_session = FALSE;
  for (guint i = 0; i < g1->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g1->nodes, i);
    if (nd->kind == BT_NODE_GHOST && strcmp(nd->label, "FutureNote") == 0) {
      assert_false(nd->broken);
      assert_string_equal(nd->relpath, "FutureNote.md");
      saw_valid_ghost = TRUE;
    }
    if (nd->kind == BT_NODE_GHOST && strcmp(nd->label, "../../escape") == 0) {
      assert_true(nd->broken);
      assert_null(nd->relpath);
      saw_broken_ghost = TRUE;
    }
    if (nd->kind == BT_NODE_SESSION)
      saw_session = TRUE;
  }
  assert_true(saw_valid_ghost);
  assert_true(saw_broken_ghost);
  assert_true(saw_session);

  /* Deterministic layout & forced brainstem (y == 260). */
  bt_graph_layout(g1, 20);
  bt_graph_layout(g2, 20);
  for (guint i = 0; i < g1->nodes->len; i++) {
    const BtGraphNode *a = g_ptr_array_index(g1->nodes, i);
    const BtGraphNode *b = g_ptr_array_index(g2->nodes, i);
    assert_true(fabs(a->x - b->x) < 1e-6);
    assert_true(fabs(a->y - b->y) < 1e-6);
    if (a->kind == BT_NODE_SESSION || a->kind == BT_NODE_IMPORT_ROOT)
      assert_true(fabs(a->y - 260.0) < 1e-6);
  }

  /* Filters reduce visible set accurately. */
  BtGraphFilter flt = {.source_filter = "human"};
  bt_graph_apply_filter(g1, &flt);
  guint vis_notes = 0;
  for (guint i = 0; i < g1->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g1->nodes, i);
    if ((nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE) && nd->visible)
      vis_notes++;
  }
  assert_int_equal(vis_notes, 2); /* isolated.md + hub.md */

  bt_graph_free(g1);
  bt_graph_free(g2);
  bt_sessions_free(ss);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_graph_invariant_ghosts_layout_and_filters),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
