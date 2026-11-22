/* bench_graph.c — Brain Graph v2 Cairo 2D benchmark:
 *   - Measures renderer init time (pre-baked radial glow sprites + vignette).
 *   - Measures 1k nodes / 3k edges (Low & High) and 3k nodes / 9k edges (Low & High).
 *   - Enforces §10 60 FPS (< 16.6 ms) budget on 1k/3k Cairo fallback. */
#include <cairo.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "graph.h"
#include "graph_state.h"
#include "graphview.h"
#include "ui/graphview_renderer.h"

#define BT_GRAPH_1K_BUDGET_MS 16.6

static BtGraph *make_bench_graph(guint n_nodes, guint n_edges)
{
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new_with_free_func(g_free);
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));

  for (guint i = 0; i < n_nodes; i++) {
    BtGraphNode *n = g_new0(BtGraphNode, 1);
    n->kind = (i % 4 == 0) ? BT_NODE_AI_NOTE : BT_NODE_HUMAN_NOTE;
    n->visible = TRUE;
    n->degree = 3;
    n->hue = (gdouble)((i * 37) % 360);
    n->radius = 6.0;
    n->birth_t = 1.0;
    g_ptr_array_add(g->nodes, n);
  }
  for (guint i = 0; i < n_edges; i++) {
    BtGraphEdge e = {
      .src_idx = i % n_nodes,
      .dst_idx = (i * 7 + 13) % n_nodes,
      .kind = BT_EDGE_LINK,
      .weight = 1.0,
    };
    g_array_append_val(g->edges, e);
  }
  bt_graph_layout(g, 10);
  return g;
}

static double run_case(BtGraphRenderer *r, cairo_t *cr, const BtGraph *g,
                       BtGraphQuality q, const char *label, guint frames)
{
  guint n = g->nodes->len;
  guint e = g->edges->len;
  g_autofree guint8 *mask = g_new0(guint8, n);
  guint hub_thresh = bt_graph_hub_degree_threshold(g);

  /* Warmup frame. */
  BtGraphRenderState st = {
    .graph = g, .width = 800, .height = 600, .scale = 1,
    .pan_x = 0.0, .pan_y = 0.0, .zoom = 1.0,
    .hover_idx = -1, .selected_idx = -1, .focus_idx = -1,
    .hover_fade = 0.0, .nbr_mask = mask, .nbr_mask_len = n,
    .hub_thresh = hub_thresh, .time_sec = 0.0,
    .quality = q, .mode = BT_GRAPH_MODE_2D, .reduce_motion = FALSE,
  };
  r->vt->render_cairo(r, cr, &st);

  gint64 t0 = g_get_monotonic_time();
  for (guint f = 0; f < frames; f++) {
    st.pan_x = (gdouble)f * 2.0;
    st.time_sec = (gdouble)f * 0.016;
    r->vt->render_cairo(r, cr, &st);
  }
  double avg_ms = ((double)(g_get_monotonic_time() - t0) / 1000.0) / (double)frames;
  double fps = avg_ms > 0.0 ? 1000.0 / avg_ms : 9999.0;
  printf("BT_BENCH %-18s renderer=cairo-%s nodes=%u edges=%u avg_ms=%.2f fps=%.1f\n",
         label, cairo_version_string(), n, e, avg_ms, fps);
  return avg_ms;
}

int main(void)
{
  gint64 ti0 = g_get_monotonic_time();
  BtGraphRenderer *r = bt_graph_cairo_renderer_new();
  double init_ms = (double)(g_get_monotonic_time() - ti0) / 1000.0;
  printf("BT_BENCH cairo_init        renderer=cairo-%s init_ms=%.2f\n",
         cairo_version_string(), init_ms);

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 800, 600);
  cairo_t *cr = cairo_create(surf);

  BtGraph *g1k = make_bench_graph(1000, 3000);
  double ms_1k_low = run_case(r, cr, g1k, BT_GRAPH_QUALITY_LOW, "cairo_1k_3k_low", 30);
  double ms_1k_high = run_case(r, cr, g1k, BT_GRAPH_QUALITY_HIGH, "cairo_1k_3k_high", 30);

  BtGraph *g3k = make_bench_graph(3000, 9000);
  (void)run_case(r, cr, g3k, BT_GRAPH_QUALITY_LOW, "cairo_3k_9k_low", 20);
  (void)run_case(r, cr, g3k, BT_GRAPH_QUALITY_HIGH, "cairo_3k_9k_high", 20);

  printf("BT_BENCH summary           1k_low_ms=%.2f 1k_high_ms=%.2f budget_1k_ms=%.1f\n",
         ms_1k_low, ms_1k_high, BT_GRAPH_1K_BUDGET_MS);

  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  g_ptr_array_unref(g1k->nodes);
  g_array_unref(g1k->edges);
  g_free(g1k);
  g_ptr_array_unref(g3k->nodes);
  g_array_unref(g3k->edges);
  g_free(g3k);
  r->vt->destroy(r);

  return ms_1k_low < BT_GRAPH_1K_BUDGET_MS ? 0 : 1;
}
