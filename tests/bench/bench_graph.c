/* bench_graph.c — §10 budget: 1,000 nodes + 3,000 edges draw pass at >= 60 fps
 * (< 16.6 ms per Cairo frame). */
#include <cairo.h>
#include <glib.h>
#include <stdio.h>

#include "graph.h"
#include "graphview.h"

#define BT_GRAPH_FRAME_BUDGET_MS 16.6

int main(void)
{
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new_with_free_func(g_free);
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));

  for (guint i = 0; i < 1000; i++) {
    BtGraphNode *n = g_new0(BtGraphNode, 1);
    n->kind = (i % 4 == 0) ? BT_NODE_AI_NOTE : BT_NODE_HUMAN_NOTE;
    n->visible = TRUE;
    n->degree = 3;
    n->hue = (gdouble)((i * 37) % 360);
    n->radius = 6.0;
    n->birth_t = 1.0;
    g_ptr_array_add(g->nodes, n);
  }
  for (guint i = 0; i < 3000; i++) {
    BtGraphEdge e = {
      .src_idx = i % 1000, .dst_idx = (i * 7 + 13) % 1000,
      .kind = BT_EDGE_LINK, .weight = 1.0,
    };
    g_array_append_val(g->edges, e);
  }
  bt_graph_layout(g, 10);

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 800, 600);
  cairo_t *cr = cairo_create(surf);

  const guint frames = 30;
  gint64 t0 = g_get_monotonic_time();
  for (guint f = 0; f < frames; f++)
    bt_graph_draw_cairo(g, cr, 800, 600, (gdouble)f * 2.0, 0.0, 1.0, -1, TRUE);
  double ms = ((double)(g_get_monotonic_time() - t0) / 1000.0) / (double)frames;

  printf("BT_BENCH graph_1k_3k nodes=1000 edges=3000 avg_ms=%.2f budget_ms=%.1f\n", ms,
         BT_GRAPH_FRAME_BUDGET_MS);

  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
  return ms < BT_GRAPH_FRAME_BUDGET_MS ? 0 : 1;
}
