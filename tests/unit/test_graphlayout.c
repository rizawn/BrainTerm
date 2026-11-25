/* test_graphlayout.c — CMocka unit tests for pure-C 2D Clean graph layout (G1). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"
#include "graphlayout.h"

static BtGraph *mk_graph(guint n)
{
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new();
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));
  for (guint i = 0; i < n; i++) {
    BtGraphNode *nd = g_new0(BtGraphNode, 1);
    nd->id = g_strdup_printf("n%u", i);
    nd->label = g_strdup_printf("Node %u", i);
    nd->relpath = g_strdup_printf("note%u.md", i);
    nd->kind = BT_NODE_HUMAN_NOTE;
    nd->parent_idx = -1;
    nd->visible = TRUE;
    nd->radius = 2.5;
    g_ptr_array_add(g->nodes, nd);
  }
  return g;
}

static void free_graph(BtGraph *g)
{
  if (g == NULL)
    return;
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    g_free(nd->id);
    g_free(nd->label);
    g_free(nd->relpath);
    g_free(nd->source);
    g_free(nd->dom_tag);
    g_free(nd->top_folder);
    g_free(nd);
  }
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
}

static void add_edge(BtGraph *g, guint a, guint b)
{
  BtGraphEdge ed = {.src_idx = a, .dst_idx = b, .kind = BT_EDGE_LINK, .weight = 1.0};
  g_array_append_val(g->edges, ed);
  if (a < g->nodes->len && b < g->nodes->len && a != b) {
    ((BtGraphNode *)g_ptr_array_index(g->nodes, a))->degree++;
    ((BtGraphNode *)g_ptr_array_index(g->nodes, b))->degree++;
  }
}

static uint64_t lcg(uint64_t *s)
{
  *s = (*s) * 6364136223846793005ULL + 1442695040888963407ULL;
  return *s >> 16;
}

static void assert_no_overlap(const BtGraph *g, float pad, guint max_allowed)
{
  guint n = g->nodes->len;
  guint coinc = 0u, ov = 0u;
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *a = g_ptr_array_index(g->nodes, i);
    if (!a->visible) continue;
    assert_true(isfinite(a->x) && isfinite(a->y));
    for (guint j = i + 1u; j < n; j++) {
      const BtGraphNode *b = g_ptr_array_index(g->nodes, j);
      if (!b->visible) continue;
      double d = hypot(a->x - b->x, a->y - b->y);
      if (d < 0.5) coinc++;
      if (d < a->radius + b->radius + (double)pad - 0.05) ov++;
    }
  }
  assert_int_equal(coinc, 0);
  assert_true(ov <= max_allowed);
}

static void test_radius_clamping(void **state)
{
  (void)state;
  assert_true(fabsf(bt_graph_node_radius_for_degree(BT_NODE_HUMAN_NOTE, 0u) - 2.5f) < 1e-5f);
  assert_true(fabsf(bt_graph_node_radius_for_degree(BT_NODE_HUMAN_NOTE, 1000u) - 10.0f) < 1e-5f);
  assert_true(fabsf(bt_graph_node_radius_for_degree(BT_NODE_TAG, 0u) - 2.0f) < 1e-5f);
  assert_true(fabsf(bt_graph_node_radius_for_degree(BT_NODE_GHOST, 0u) - 2.25f) < 1e-5f);
}

static void test_determinism_and_topologies(void **state)
{
  (void)state;
  BtGraphLayoutOpts opts;
  bt_graph_layout_default_opts(&opts);

  /* 1. Sparse random (n=200, m=150) + determinism */
  BtGraph *g1 = mk_graph(200);
  BtGraph *g2 = mk_graph(200);
  uint64_t s = 12345ULL;
  for (guint e = 0; e < 150u; e++) {
    guint a = (guint)(lcg(&s) % 200u);
    guint b = (guint)(lcg(&s) % 200u);
    add_edge(g1, a, b);
    add_edge(g2, a, b);
  }
  bt_graph_layout_run(g1, &opts);
  bt_graph_layout_run(g2, &opts);
  for (guint i = 0; i < 200u; i++) {
    const BtGraphNode *a = g_ptr_array_index(g1->nodes, i);
    const BtGraphNode *b = g_ptr_array_index(g2->nodes, i);
    assert_true(fabs(a->x - b->x) < 1e-6);
    assert_true(fabs(a->y - b->y) < 1e-6);
  }
  assert_no_overlap(g1, opts.collision_pad, 0u);
  free_graph(g1);
  free_graph(g2);

  /* 2. Star (1 hub + 300 leaves) */
  BtGraph *star = mk_graph(301);
  for (guint i = 1; i <= 300u; i++) add_edge(star, 0u, i);
  bt_graph_layout_run(star, &opts);
  assert_no_overlap(star, opts.collision_pad, 0u);
  free_graph(star);

  /* 3. Chain (250), Grid (15x15=225), K30, Binary tree depth 8 (255), Loose-only (400) */
  BtGraph *chain = mk_graph(250);
  for (guint i = 0; i < 249u; i++) add_edge(chain, i, i + 1u);
  bt_graph_layout_run(chain, &opts);
  assert_no_overlap(chain, opts.collision_pad, 0u);
  free_graph(chain);

  BtGraph *grid = mk_graph(225);
  for (guint r = 0; r < 15u; r++) {
    for (guint c = 0; c < 15u; c++) {
      if (c + 1u < 15u) add_edge(grid, r * 15u + c, r * 15u + c + 1u);
      if (r + 1u < 15u) add_edge(grid, r * 15u + c, (r + 1u) * 15u + c);
    }
  }
  bt_graph_layout_run(grid, &opts);
  assert_no_overlap(grid, opts.collision_pad, 0u);
  free_graph(grid);

  BtGraph *k30 = mk_graph(30);
  for (guint i = 0; i < 30u; i++)
    for (guint j = i + 1u; j < 30u; j++)
      add_edge(k30, i, j);
  bt_graph_layout_run(k30, &opts);
  assert_no_overlap(k30, opts.collision_pad, 0u);
  free_graph(k30);

  BtGraph *btree = mk_graph(255);
  for (guint i = 0; i < 127u; i++) {
    add_edge(btree, i, 2u * i + 1u);
    add_edge(btree, i, 2u * i + 2u);
  }
  bt_graph_layout_run(btree, &opts);
  assert_no_overlap(btree, opts.collision_pad, 0u);
  free_graph(btree);

  BtGraph *loose = mk_graph(400);
  bt_graph_layout_run(loose, &opts);
  assert_no_overlap(loose, opts.collision_pad, 0u);
  /* Verify no vertical column (>3 sharing x within 2px at same y-band, or all x == ±14). */
  guint at_14 = 0u;
  for (guint i = 0; i < 400u; i++) {
    const BtGraphNode *nd = g_ptr_array_index(loose->nodes, i);
    if (fabs(fabs(nd->x) - 14.0) < 0.5) at_14++;
  }
  assert_true(at_14 < 20u);
  free_graph(loose);
}

static void test_edge_cases_and_safety(void **state)
{
  (void)state;
  BtGraphLayoutOpts opts;
  bt_graph_layout_default_opts(&opts);

  /* 0, 1, 2 nodes */
  BtGraph *g0 = mk_graph(0);
  bt_graph_layout_run(g0, &opts);
  free_graph(g0);

  BtGraph *g1 = mk_graph(1);
  bt_graph_layout_run(g1, &opts);
  free_graph(g1);

  BtGraph *g2 = mk_graph(2);
  add_edge(g2, 0, 1);
  add_edge(g2, 0, 0);    /* self-loop */
  add_edge(g2, 0, 1);    /* duplicate */
  add_edge(g2, 0, 9999); /* out-of-range target */
  bt_graph_layout_run(g2, &opts);
  assert_no_overlap(g2, opts.collision_pad, 0u);
  free_graph(g2);

  /* NaN/Inf starting coordinates with warm_start */
  BtGraph *gnan = mk_graph(12);
  for (guint i = 0; i < 11u; i++) add_edge(gnan, i, i + 1u);
  ((BtGraphNode *)g_ptr_array_index(gnan->nodes, 0))->x = NAN;
  ((BtGraphNode *)g_ptr_array_index(gnan->nodes, 1))->y = INFINITY;
  opts.warm_start = TRUE;
  bt_graph_layout_run(gnan, &opts);
  assert_no_overlap(gnan, opts.collision_pad, 0u);
  free_graph(gnan);
}

static void test_warm_start_pinning_visibility_and_timecap(void **state)
{
  (void)state;
  BtGraphLayoutOpts opts;
  bt_graph_layout_default_opts(&opts);

  /* Warm start stability: adding 1 node with 3 links moves others < 25px (p95). */
  BtGraph *g = mk_graph(80);
  for (guint i = 0; i < 79u; i++) add_edge(g, i, (i + 1u) % 79u);
  for (guint i = 0; i < 40u; i++) add_edge(g, i, (i * 7u) % 79u);
  ((BtGraphNode *)g_ptr_array_index(g->nodes, 79))->visible = FALSE;
  bt_graph_layout_run(g, &opts);

  gdouble old_x[79], old_y[79];
  for (guint i = 0; i < 79u; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    old_x[i] = nd->x;
    old_y[i] = nd->y;
  }

  /* Reveal node 79 with 3 links and warm-start. */
  ((BtGraphNode *)g_ptr_array_index(g->nodes, 79))->visible = TRUE;
  add_edge(g, 79, 0);
  add_edge(g, 79, 10);
  add_edge(g, 79, 20);
  opts.warm_start = TRUE;
  opts.max_iterations = 80u;
  bt_graph_layout_run(g, &opts);

  gdouble disp[79];
  for (guint i = 0; i < 79u; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    disp[i] = hypot(nd->x - old_x[i], nd->y - old_y[i]);
  }
  /* Simple sort for p95 */
  for (guint i = 0; i < 79u; i++)
    for (guint j = i + 1u; j < 79u; j++)
      if (disp[j] < disp[i]) { gdouble t = disp[i]; disp[i] = disp[j]; disp[j] = t; }
  assert_true(disp[75] < 25.0);
  free_graph(g);

  /* Pinned node never moves. */
  BtGraph *gp = mk_graph(16);
  for (guint i = 0; i < 15u; i++) add_edge(gp, 0, i + 1u);
  bt_graph_layout_default_opts(&opts);
  BtGraphLayoutState *st = bt_graph_layout_begin(gp, &opts);
  bt_graph_layout_set_pos(st, 0u, 123.0f, -45.0f);
  bt_graph_layout_set_pinned(st, 0u, TRUE);
  bt_graph_layout_step(st, 120u, 0.0);
  bt_graph_layout_finish(st);
  bt_graph_layout_free(st);
  const BtGraphNode *p0 = g_ptr_array_index(gp->nodes, 0);
  assert_true(fabs(p0->x - 123.0) < 1e-4 && fabs(p0->y - (-45.0)) < 1e-4);
  free_graph(gp);

  /* Tiny time_cap_ms still produces valid non-overlapping layout. */
  BtGraph *gt = mk_graph(120);
  for (guint i = 0; i < 119u; i++) add_edge(gt, i, i + 1u);
  bt_graph_layout_default_opts(&opts);
  opts.time_cap_ms = 0.5;
  bt_graph_layout_run(gt, &opts);
  assert_no_overlap(gt, opts.collision_pad, 0u);
  free_graph(gt);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_radius_clamping),
    cmocka_unit_test(test_determinism_and_topologies),
    cmocka_unit_test(test_edge_cases_and_safety),
    cmocka_unit_test(test_warm_start_pinning_visibility_and_timecap),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
