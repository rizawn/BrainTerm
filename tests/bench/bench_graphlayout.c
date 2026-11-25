/* bench_graphlayout.c — Headless performance benchmark for Clean 2D graph layout
 * at 1,000 and 3,000 nodes (Section 5.9 & Section 9). */
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "graph.h"
#include "graphlayout.h"

static uint64_t lcg(uint64_t *s)
{
  *s = (*s) * 6364136223846793005ULL + 1442695040888963407ULL;
  return *s >> 16;
}

static BtGraph *make_bench_graph(guint n, guint m)
{
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new();
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));
  for (guint i = 0; i < n; i++) {
    BtGraphNode *nd = g_new0(BtGraphNode, 1);
    nd->id = g_strdup_printf("b%u", i);
    nd->label = g_strdup_printf("Note %u", i);
    nd->relpath = g_strdup_printf("bench/n%u.md", i);
    nd->kind = BT_NODE_HUMAN_NOTE;
    nd->parent_idx = -1;
    nd->visible = TRUE;
    nd->radius = 2.5;
    g_ptr_array_add(g->nodes, nd);
  }
  uint64_t rng = 0xBEEF1234ULL ^ (uint64_t)n;
  /* 75% in main cluster + 10% small islands + 15% loose halo notes */
  guint main_n = (n * 75u) / 100u;
  for (guint e = 0; e < m; e++) {
    guint a = (guint)(lcg(&rng) % main_n);
    guint b = (guint)(lcg(&rng) % main_n);
    if (a == b) continue;
    BtGraphEdge ed = {.src_idx = a, .dst_idx = b, .kind = BT_EDGE_LINK, .weight = 1.0};
    g_array_append_val(g->edges, ed);
    ((BtGraphNode *)g_ptr_array_index(g->nodes, a))->degree++;
    ((BtGraphNode *)g_ptr_array_index(g->nodes, b))->degree++;
  }
  return g;
}

static void free_bench_graph(BtGraph *g)
{
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    g_free(nd->id); g_free(nd->label); g_free(nd->relpath); g_free(nd);
  }
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
}

static double run_case(guint n, guint m, guint *iters_out, guint *ov_out)
{
  BtGraph *g = make_bench_graph(n, m);
  BtGraphLayoutOpts opts;
  bt_graph_layout_default_opts(&opts);

  gint64 t0 = g_get_monotonic_time();
  BtGraphLayoutState *st = bt_graph_layout_begin(g, &opts);
  bt_graph_layout_step(st, opts.max_iterations, 0.0);
  bt_graph_layout_finish(st);
  double ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
  if (iters_out) *iters_out = bt_graph_layout_iterations_run(st);
  if (ov_out) *ov_out = bt_graph_layout_remaining_overlaps(st);
  bt_graph_layout_free(st);
  free_bench_graph(g);
  return ms;
}

int main(void)
{
  guint it1 = 0, ov1 = 0, it3 = 0, ov3 = 0;
  double ms1 = run_case(1000u, 1200u, &it1, &ov1);
  double ms3 = run_case(3000u, 4000u, &it3, &ov3);
  printf("bench_graphlayout 1k (n=1000, m=1200): %.2f ms  iters=%u  rem_overlaps=%u\n", ms1, it1, ov1);
  printf("bench_graphlayout 3k (n=3000, m=4000): %.2f ms  iters=%u  rem_overlaps=%u\n", ms3, it3, ov3);
  return 0;
}
