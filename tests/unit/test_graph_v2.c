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
#include "graph_palette.h"
#include "graph_state.h"
#include "graph_timeline.h"

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

  /* Hidden neighbor excluded from mask */
  ((BtGraphNode *)g_ptr_array_index(g->nodes, 2))->visible = FALSE;
  bt_graph_compute_neighbors(g, 1, mask, 6);
  assert_int_equal(mask[0], BT_NBR_NEIGHBOR);
  assert_int_equal(mask[1], BT_NBR_FOCUS);
  assert_int_equal(mask[2], BT_NBR_NONE);

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

  /* Hub visible at zoom 1.0, suppressed when dimmed below 40% during hover, hidden when far out */
  gdouble a_hub_idle = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 1.0, FALSE, 0.0, 60);
  gdouble a_hub_mid = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 1.0, TRUE, 0.4, 60);
  gdouble a_hub_dim = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 1.0, TRUE, 1.0, 60);
  gdouble a_hub_far = bt_graph_label_alpha(hub, BT_NBR_NONE, FALSE, FALSE, thresh, 0.35, FALSE, 0.0, 60);
  assert_true(a_hub_idle > 0.70);
  assert_true(a_hub_mid >= 0.40);
  assert_true(a_hub_dim <= 0.001);
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

static void test_label_placement_no_overlap_and_caps(void **state)
{
  (void)state;
  assert_int_equal(bt_graph_label_cap_for_zoom(1.0), 20u);

  /* 1. Crowded overlapping cluster + cap enforcement */
  BtGraphLabelCand cands[40];
  memset(cands, 0, sizeof(cands));
  for (guint i = 0; i < 40; i++) {
    cands[i].node_idx = i;
    cands[i].x = (float)((i % 5) * 120);
    cands[i].y = (float)((i / 5) * 28);
    cands[i].w = 100.0f;
    cands[i].h = 18.0f;
    cands[i].alpha = 0.8f;
    cands[i].degree = 40u - i;
    cands[i].priority = 1u;
  }
  /* Force half of them onto colliding coordinates */
  for (guint i = 10; i < 20; i++) {
    cands[i].x = cands[0].x + 5.0f;
    cands[i].y = cands[0].y + 2.0f;
  }
  /* Mark node 39 as focused and node 38 as selected (must always appear) */
  cands[39].priority = 5u;
  cands[39].always_show = TRUE;
  cands[39].x = cands[0].x;
  cands[39].y = cands[0].y;
  cands[38].priority = 4u;
  cands[38].always_show = TRUE;
  cands[38].x = cands[0].x;
  cands[38].y = cands[0].y;

  BtGraphCoreObstacle obs[1] = {{.cx = 290.0f, .cy = 9.0f, .r = 8.0f, .node_idx = 999u}};
  BtGraphLabelCand out[64];
  guint n = bt_graph_place_labels(cands, 40u, obs, 1u, 20u, 12u, out, 64u);
  assert_true(n <= 20u);
  assert_true(n >= 2u);
  /* Focused (39) and selected (38) are placed first */
  assert_int_equal(out[0].node_idx, 39u);
  assert_int_equal(out[1].node_idx, 38u);

  /* Zero pairwise AABB overlaps across all placed boxes */
  for (guint i = 0; i < n; i++) {
    for (guint j = i + 1; j < n; j++) {
      gboolean ov = (out[i].x < out[j].x + out[j].w) && (out[i].x + out[i].w > out[j].x) &&
                    (out[i].y < out[j].y + out[j].h) && (out[i].y + out[i].h > out[j].y);
      assert_false(ov);
    }
  }

  /* 2. Hub hover neighbor cap: 25 non-overlapping neighbors -> at most 12 placed, highest degree */
  BtGraphLabelCand nbrs[26];
  memset(nbrs, 0, sizeof(nbrs));
  nbrs[0].node_idx = 100u;
  nbrs[0].x = 10.0f; nbrs[0].y = 10.0f; nbrs[0].w = 80.0f; nbrs[0].h = 16.0f;
  nbrs[0].alpha = 1.0f; nbrs[0].degree = 70u; nbrs[0].priority = 3u; nbrs[0].always_show = TRUE;
  for (guint i = 1; i < 26; i++) {
    nbrs[i].node_idx = 100u + i;
    nbrs[i].x = (float)((i % 5) * 140);
    nbrs[i].y = (float)((i / 5 + 1) * 40);
    nbrs[i].w = 90.0f; nbrs[i].h = 16.0f;
    nbrs[i].alpha = 0.9f;
    nbrs[i].degree = i; /* highest degree = 25 down to 14 */
    nbrs[i].priority = 2u;
    nbrs[i].is_neighbor = TRUE;
  }
  guint nn = bt_graph_place_labels(nbrs, 26u, NULL, 0u, 20u, 12u, out, 64u);
  assert_int_equal(nn, 13u); /* 1 hovered + 12 highest-degree neighbors */
  assert_int_equal(out[0].node_idx, 100u);
  assert_int_equal(out[1].degree, 25u);
  assert_int_equal(out[12].degree, 14u);
}

static void test_cursor_anchored_zoom(void **state)
{
  (void)state;
  const gdouble vp_w = 1000.0, vp_h = 700.0;
  gdouble pan_x = 42.0, pan_y = -18.0, zoom = 1.0;
  /* Suppose a node is at world (wx, wy) = (120.0, -75.0) */
  const gdouble wx = 120.0, wy = -75.0;
  gdouble mx = vp_w * 0.5 + pan_x + wx * zoom;
  gdouble my = vp_h * 0.5 + pan_y + wy * zoom;

  /* Scroll in 5 steps and back out 3 steps on that node */
  for (int i = 0; i < 5; i++)
    bt_graph_zoom_at_point(&pan_x, &pan_y, &zoom, zoom * 1.12, mx, my, vp_w, vp_h);
  for (int i = 0; i < 3; i++)
    bt_graph_zoom_at_point(&pan_x, &pan_y, &zoom, zoom * 0.89, mx, my, vp_w, vp_h);

  gdouble after_sx = vp_w * 0.5 + pan_x + wx * zoom;
  gdouble after_sy = vp_h * 0.5 + pan_y + wy * zoom;
  assert_true(fabs(after_sx - mx) < 1e-6);
  assert_true(fabs(after_sy - my) < 1e-6);
}

static void test_spatial_pick_stack_and_spread(void **state)
{
  (void)state;
  BtScreenNode nodes[4] = {
    {.node_idx = 0, .sx = 200.0f, .sy = 150.0f, .radius = 5.0f, .depth = 3.0f, .visible = TRUE},
    {.node_idx = 1, .sx = 202.0f, .sy = 150.0f, .radius = 5.0f, .depth = 1.5f, .visible = TRUE},
    {.node_idx = 2, .sx = 200.0f, .sy = 151.0f, .radius = 5.0f, .depth = 1.5f, .visible = TRUE},
    {.node_idx = 3, .sx = 400.0f, .sy = 400.0f, .radius = 5.0f, .depth = 1.0f, .visible = TRUE},
  };
  BtPickCand out[8];
  guint cnt = bt_graph_pick_screen(nodes, 4u, 800.0f, 600.0f, 200.0f, 150.0f, out, 8u);
  assert_int_equal(cnt, 3u);
  /* Front-most (depth 1.5), tie-broken by closest to cursor (node 2 dist2=1 < node 1 dist2=4). */
  assert_int_equal(out[0].node_idx, 2u);
  assert_int_equal(out[1].node_idx, 1u);
  assert_int_equal(out[2].node_idx, 0u);

  /* Hover spread pushes nodes within 40px away from cursor and leaves far nodes untouched. */
  bt_graph_apply_hover_spread(nodes, 4u, 200.0f, 150.0f, 40.0f, 1.0f);
  assert_true(nodes[1].sx > 202.0f);
  assert_true(nodes[2].sy > 151.0f);
  assert_true(fabsf(nodes[3].sx - 400.0f) < 1e-5f);
}

static void test_galaxy_palette_assignment_and_persistence(void **state)
{
  (void)state;
  gchar *tmp = g_dir_make_tmp("bt-pal-XXXXXX", NULL);
  BtFolderColors *fc = bt_folder_colors_load(tmp);

  /* Build a synthetic graph with 15 top-level folders (exceeding 12 swatches). */
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new_with_free_func(g_free);
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));
  for (guint i = 0; i < 15u; i++) {
    BtGraphNode *nd = g_new0(BtGraphNode, 1);
    nd->id = g_strdup_printf("f%u", i);
    nd->label = g_strdup_printf("Folder%02u", i);
    nd->relpath = g_strdup(nd->label);
    nd->top_folder = g_strdup(nd->label);
    nd->kind = BT_NODE_FOLDER;
    nd->parent_idx = -1;
    nd->visible = TRUE;
    g_ptr_array_add(g->nodes, nd);
  }

  assert_true(bt_folder_colors_apply(fc, g, TRUE, 42ULL));

  /* First 12 folders must all have distinct colors (no duplicates before exhaustion). */
  guint32 cols[15];
  for (guint i = 0; i < 15u; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    assert_true(nd->color_rgb != 0u);
    cols[i] = nd->color_rgb;
  }
  for (guint i = 0; i < 12u; i++) {
    for (guint j = i + 1; j < 12u; j++)
      assert_true(cols[i] != cols[j]);
  }
  /* Generated 13th..15th colors must be perceptually separated from prior colors. */
  for (guint i = 12u; i < 15u; i++) {
    for (guint j = 0; j < i; j++)
      assert_true(bt_color_perceptual_dist(cols[i], cols[j]) > 25.0);
  }

  /* Save & reload round-trip from <vault>/.brainterm/folder-colors.json. */
  assert_true(bt_folder_colors_save(fc, NULL));
  BtFolderColors *fc2 = bt_folder_colors_load(tmp);
  for (guint i = 0; i < 15u; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    assert_int_equal(bt_folder_colors_get(fc2, nd->top_folder), cols[i]);
  }

  bt_folder_colors_free(fc2);
  bt_folder_colors_free(fc);
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    g_free(nd->id); g_free(nd->label); g_free(nd->relpath); g_free(nd->top_folder);
  }
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
  g_free(tmp);
}

static void test_galaxy_layout_determinism_and_z_bulge(void **state)
{
  (void)state;
  BtGraph *g = make_sample_graph(120);
  /* Assign 6 top-level folders, make first 10 high-degree hubs, last 10 ghosts. */
  for (guint i = 0; i < 120u; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (i < 10u) {
      nd->degree = 25u;
      nd->top_folder = NULL; /* bulge hub */
    } else if (i >= 110u) {
      nd->kind = BT_NODE_GHOST;
      nd->top_folder = NULL;
    } else {
      nd->top_folder = g_strdup_printf("Folder%u", i % 6u);
      nd->relpath = g_strdup_printf("Folder%u/note%u.md", i % 6u, i);
    }
  }

  BtGraphVec3 a[120], b[120];
  bt_graph_compute_galaxy_layout(g, 4u, a, 120u);
  bt_graph_compute_galaxy_layout(g, 4u, b, 120u);

  /* Strict determinism: same input -> identical output. */
  for (guint i = 0; i < 120u; i++) {
    assert_true(fabsf(a[i].x - b[i].x) < 1e-6f);
    assert_true(fabsf(a[i].y - b[i].y) < 1e-6f);
    assert_true(fabsf(a[i].z - b[i].z) < 1e-6f);
  }

  /* Vertical z-distribution: core bulge (r < 0.25) thicker on average than outer disk (r > 0.50). */
  gdouble bulge_z = 0.0, disk_z = 0.0;
  guint bulge_n = 0u, disk_n = 0u;
  for (guint i = 0; i < 120u; i++) {
    gdouble r = hypot((gdouble)a[i].x, (gdouble)a[i].z);
    if (r < 0.28) { bulge_z += fabs((gdouble)a[i].y); bulge_n++; }
    else if (r > 0.45) { disk_z += fabs((gdouble)a[i].y); disk_n++; }
  }
  assert_true(bulge_n > 0u && disk_n > 0u);
  assert_true((bulge_z / (gdouble)bulge_n) > (disk_z / (gdouble)disk_n));

  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    g_free(nd->top_folder);
    g_free(nd->relpath);
  }
  free_sample_graph(g);
}

static void test_timeline_date_parse_and_lane_packing(void **state)
{
  (void)state;
  gint64 t1 = bt_timeline_parse_date("2024-06-15");
  gint64 t2 = bt_timeline_parse_date("2025-01-10T08:30:00Z");
  assert_true(t1 > 0);
  assert_true(t2 > t1);
  assert_int_equal(bt_timeline_parse_date("not-a-date"), 0);

  /* Build 24 notes with identical timestamps in the same folder to stress packing! */
  BtGraph *g = make_sample_graph(24);
  for (guint i = 0; i < 24u; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    nd->created = t1 + (gint64)(i % 3u) * 86400;
    nd->top_folder = g_strdup((i < 12u) ? "Alpha" : "Beta");
  }

  BtTimelineLayout *tl = bt_timeline_build(g, BT_TL_LANE_FOLDER, 60.0f, 860.0f, 80.0f, 440.0f, 0, 0);
  assert_non_null(tl);
  assert_int_equal(tl->lane_count, 2u);
  assert_true(tl->event_count >= 1u);

  /* Verify Kept grid has ZERO overlapping dots. */
  for (guint i = 0; i < tl->dot_count; i++) {
    if (!tl->dots[i].valid) continue;
    for (guint j = i + 1; j < tl->dot_count; j++) {
      if (!tl->dots[j].valid) continue;
      float dx = tl->dots[i].kept_x - tl->dots[j].kept_x;
      float dy = tl->dots[i].kept_y - tl->dots[j].kept_y;
      float d = hypotf(dx, dy);
      assert_true(d >= tl->dots[i].radius + tl->dots[j].radius);
    }
  }

  bt_timeline_free(tl);
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    g_free(nd->top_folder);
  }
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
    cmocka_unit_test(test_label_placement_no_overlap_and_caps),
    cmocka_unit_test(test_cursor_anchored_zoom),
    cmocka_unit_test(test_spatial_pick_stack_and_spread),
    cmocka_unit_test(test_galaxy_palette_assignment_and_persistence),
    cmocka_unit_test(test_galaxy_layout_determinism_and_z_bulge),
    cmocka_unit_test(test_timeline_date_parse_and_lane_packing),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
