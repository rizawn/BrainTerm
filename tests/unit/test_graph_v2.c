/* test_graph_v2.c — CMocka tests for Brain Graph v2 core logic (Step 1a):
 *   1. Renderer selection & GL fallback rules (no GL context, old GL/GLES, shader fail, FBO fail, env force).
 *   2. Hover/selection 1-hop neighbor bitmask computation (zero alloc, self-loop, bounds).
 *   3. Label visibility & fade alpha rules (hover, neighbor, selected, kbd focus, hub threshold, zoom).
 *   4. Deterministic 3D cortex/brainstem layout bounds & separation. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <math.h>
#include <string.h>

#include "graph.h"
#include "graph_state.h"

static BtGraph *make_sample_graph(guint n_nodes)
{
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new_with_free_func(g_free);
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));

  for (guint i = 0; i < n_nodes; i++) {
    BtGraphNode *nd = g_new0(BtGraphNode, 1);
    nd->id = g_strdup_printf("n%u", i);
    nd->label = g_strdup_printf("Note %u", i);
    nd->kind = (i % 5 == 0) ? BT_NODE_AI_NOTE : BT_NODE_HUMAN_NOTE;
    nd->visible = TRUE;
    nd->radius = 6.0;
    nd->birth_t = 1.0;
    nd->x = (gdouble)((gint)(i % 10) - 5) * 30.0;
    nd->y = (gdouble)((gint)(i / 10) - 5) * 30.0;
    g_ptr_array_add(g->nodes, nd);
  }
  return g;
}

static void free_sample_graph(BtGraph *g)
{
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    g_free(nd->id);
    g_free(nd->label);
  }
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
}

static void add_edge(BtGraph *g, guint a, guint b)
{
  BtGraphEdge e = {.src_idx = a, .dst_idx = b, .kind = BT_EDGE_LINK, .weight = 1.0};
  g_array_append_val(g->edges, e);
  ((BtGraphNode *)g_ptr_array_index(g->nodes, a))->degree++;
  ((BtGraphNode *)g_ptr_array_index(g->nodes, b))->degree++;
}

static void test_backend_select_gl_and_gles(void **state)
{
  (void)state;
  BtGraphBackendDecision d;

  BtGraphGlProbe gl33 = {
    .gl_context_ok = TRUE, .is_gles = FALSE, .gl_major = 3, .gl_minor = 3,
    .shaders_ok = TRUE, .fbo_ok = TRUE, .force_cairo_env = FALSE,
  };
  bt_graph_select_backend(&gl33, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_GL);
  assert_true(d.bloom_enabled);
  assert_true(d.mode_3d_available);
  assert_string_equal(d.reason, "");

  BtGraphGlProbe gles30 = {
    .gl_context_ok = TRUE, .is_gles = TRUE, .gl_major = 3, .gl_minor = 0,
    .shaders_ok = TRUE, .fbo_ok = TRUE, .force_cairo_env = FALSE,
  };
  bt_graph_select_backend(&gles30, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_GL);
  assert_true(d.bloom_enabled);
  assert_true(d.mode_3d_available);
}

static void test_backend_select_fallbacks(void **state)
{
  (void)state;
  BtGraphBackendDecision d;

  /* 1. No GL context (old driver / RDP / headless) -> Cairo fallback */
  BtGraphGlProbe no_ctx = {.gl_context_ok = FALSE};
  bt_graph_select_backend(&no_ctx, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_CAIRO);
  assert_false(d.bloom_enabled);
  assert_false(d.mode_3d_available);
  assert_non_null(strstr(d.reason, "context"));

  /* 2. Desktop GL 3.2 (too old) -> Cairo fallback */
  BtGraphGlProbe old_gl = {
    .gl_context_ok = TRUE, .is_gles = FALSE, .gl_major = 3, .gl_minor = 2,
    .shaders_ok = TRUE, .fbo_ok = TRUE,
  };
  bt_graph_select_backend(&old_gl, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_CAIRO);
  assert_non_null(strstr(d.reason, "3.2"));

  /* 3. GLES 2.0 (too old) -> Cairo fallback */
  BtGraphGlProbe old_es = {
    .gl_context_ok = TRUE, .is_gles = TRUE, .gl_major = 2, .gl_minor = 0,
    .shaders_ok = TRUE, .fbo_ok = TRUE,
  };
  bt_graph_select_backend(&old_es, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_CAIRO);
  assert_non_null(strstr(d.reason, "ES 2.0"));

  /* 4. Shader link failure -> Cairo fallback */
  BtGraphGlProbe bad_sh = {
    .gl_context_ok = TRUE, .gl_major = 4, .gl_minor = 5,
    .shaders_ok = FALSE, .fbo_ok = TRUE,
  };
  bt_graph_select_backend(&bad_sh, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_CAIRO);
  assert_non_null(strstr(d.reason, "shader"));

  /* 5. FBO unsupported -> stay on GL, downgrade bloom off */
  BtGraphGlProbe no_fbo = {
    .gl_context_ok = TRUE, .gl_major = 4, .gl_minor = 6,
    .shaders_ok = TRUE, .fbo_ok = FALSE,
  };
  bt_graph_select_backend(&no_fbo, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_GL);
  assert_false(d.bloom_enabled);
  assert_true(d.mode_3d_available);
  assert_non_null(strstr(d.reason, "FBO"));

  /* 6. Forced Cairo env override */
  BtGraphGlProbe forced = {
    .gl_context_ok = TRUE, .gl_major = 4, .gl_minor = 6,
    .shaders_ok = TRUE, .fbo_ok = TRUE, .force_cairo_env = TRUE,
  };
  bt_graph_select_backend(&forced, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_CAIRO);
  assert_non_null(strstr(d.reason, "BRAINTERM_GRAPH_BACKEND"));

  /* 7. NULL probe safe */
  bt_graph_select_backend(NULL, &d);
  assert_int_equal(d.backend, BT_GRAPH_BACKEND_CAIRO);
}

static void test_hover_neighbors(void **state)
{
  (void)state;
  BtGraph *g = make_sample_graph(6);
  /* Chain 0 - 1 - 2 - 3; node 4 isolated; self-loop on 1. */
  add_edge(g, 0, 1);
  add_edge(g, 1, 2);
  add_edge(g, 2, 3);
  add_edge(g, 1, 1);

  guint8 mask[6];
  bt_graph_compute_neighbors(g, 1, mask, 6);
  assert_int_equal(mask[0], BT_NBR_NEIGHBOR);
  assert_int_equal(mask[1], BT_NBR_FOCUS);
  assert_int_equal(mask[2], BT_NBR_NEIGHBOR);
  assert_int_equal(mask[3], BT_NBR_NONE); /* 2-hop excluded */
  assert_int_equal(mask[4], BT_NBR_NONE); /* isolated */
  assert_int_equal(mask[5], BT_NBR_NONE);

  /* Isolated node -> only itself marked */
  bt_graph_compute_neighbors(g, 4, mask, 6);
  assert_int_equal(mask[4], BT_NBR_FOCUS);
  assert_int_equal(mask[0], BT_NBR_NONE);
  assert_int_equal(mask[1], BT_NBR_NONE);

  /* Invalid / negative focus -> all zeroed */
  bt_graph_compute_neighbors(g, -1, mask, 6);
  for (guint i = 0; i < 6; i++)
    assert_int_equal(mask[i], BT_NBR_NONE);

  bt_graph_compute_neighbors(g, 99, mask, 6);
  for (guint i = 0; i < 6; i++)
    assert_int_equal(mask[i], BT_NBR_NONE);

  free_sample_graph(g);
}

static void test_label_visibility_rules(void **state)
{
  (void)state;
  BtGraph *g = make_sample_graph(60);
  /* Give node 0 high degree (hub), keep node 10 degree=1. */
  for (guint i = 1; i <= 12; i++)
    add_edge(g, 0, i);

  guint thresh = bt_graph_hub_degree_threshold(g);
  assert_true(thresh >= 3u);

  const BtGraphNode *hub = g_ptr_array_index(g->nodes, 0);
  const BtGraphNode *leaf = g_ptr_array_index(g->nodes, 10);

  /* Hovered / selected / keyboard-focused always show cleanly */
  gdouble a_hov = bt_graph_label_alpha(leaf, BT_NBR_FOCUS, FALSE, FALSE, thresh, 0.3, TRUE, 1.0, 60);
  assert_true(a_hov >= 0.99);
  gdouble a_sel = bt_graph_label_alpha(leaf, BT_NBR_NONE, TRUE, FALSE, thresh, 0.3, FALSE, 0.0, 60);
  assert_true(a_sel >= 0.99);
  gdouble a_kbd = bt_graph_label_alpha(leaf, BT_NBR_NONE, FALSE, TRUE, thresh, 0.3, FALSE, 0.0, 60);
  assert_true(a_kbd >= 0.99);

  /* 1-hop neighbor shows at full fade */
  gdouble a_nbr = bt_graph_label_alpha(leaf, BT_NBR_NEIGHBOR, FALSE, FALSE, thresh, 1.0, TRUE, 1.0, 60);
  assert_true(a_nbr >= 0.85);

  /* Non-hub leaf hidden in large graph when not focused */
  gdouble a_leaf_idle = bt_graph_label_alpha(leaf, BT_NBR_NONE, FALSE, FALSE, thresh, 1.0, FALSE, 0.0, 60);
  assert_true(a_leaf_idle <= 0.001);

  /* Hub visible at zoom 1.0, dimmed to 25% when another node is hovered, hidden when zoomed far out */
  gdouble a_hub_idle = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 1.0, FALSE, 0.0, 60);
  gdouble a_hub_dim = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 1.0, TRUE, 1.0, 60);
  gdouble a_hub_far = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 0.35, FALSE, 0.0, 60);
  assert_true(a_hub_idle > 0.70);
  assert_true(fabs(a_hub_dim - a_hub_idle * 0.25) < 0.02);
  assert_true(a_hub_far <= 0.001);

  free_sample_graph(g);
}

static void test_3d_layout_deterministic_and_bounded(void **state)
{
  (void)state;
  BtGraph *g = make_sample_graph(40);
  for (guint i = 0; i < 30; i++)
    add_edge(g, i, (i + 3) % 30);
  /* Mark node 35 as brainstem session, node 38 as isolated spine note. */
  ((BtGraphNode *)g_ptr_array_index(g->nodes, 35))->kind = BT_NODE_SESSION;
  ((BtGraphNode *)g_ptr_array_index(g->nodes, 38))->degree = 0;

  BtGraphVec3 a[40], b[40];
  bt_graph_compute_3d_layout(g, a, 40);
  bt_graph_compute_3d_layout(g, b, 40);

  for (guint i = 0; i < 40; i++) {
    assert_true(isfinite(a[i].x) && isfinite(a[i].y) && isfinite(a[i].z));
    assert_true(fabsf(a[i].x) <= 1.01f && fabsf(a[i].y) <= 1.01f && fabsf(a[i].z) <= 1.01f);
    assert_true(a[i].x == b[i].x && a[i].y == b[i].y && a[i].z == b[i].z);
  }
  /* Brainstem session sits below the cortex nodes. */
  assert_true(a[35].y < -0.60f);

  free_sample_graph(g);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_backend_select_gl_and_gles),
    cmocka_unit_test(test_backend_select_fallbacks),
    cmocka_unit_test(test_hover_neighbors),
    cmocka_unit_test(test_label_visibility_rules),
    cmocka_unit_test(test_3d_layout_deterministic_and_bounded),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
