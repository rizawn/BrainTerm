/* bench_graph_gl.c — Brain Graph v2 OpenGL 2D + Bloom + 3D Orbit benchmark (Step 3):
 *   - Realizes GtkGLArea, reports GL_VENDOR / GL_RENDERER / GL_VERSION + init_ms.
 *   - Measures 800x600 offscreen FBO frame time (60 frames + glFinish) for:
 *       2D 1k/3k Low (no bloom), 2D 1k/3k High (with half-res Gaussian bloom),
 *       2D 3k/9k Low (no bloom), 2D 3k/9k High (with bloom),
 *       3D 1k/3k High (orbit + bloom), 3D 3k/9k High (orbit + bloom).
 *   - Verifies non-uniform rendered pixels for both 2D bloom and 3D bloom.
 *   - Exits 77 (Meson skip) cleanly if no display or GL context is unavailable. */
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "graph.h"
#include "graph_state.h"
#include "ui/graphgl.h"

static BtGraph *make_bench_graph(guint n_nodes, guint n_edges)
{
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new_with_free_func(g_free);
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));

  for (guint i = 0; i < n_nodes; i++) {
    BtGraphNode *n = g_new0(BtGraphNode, 1);
    n->id = g_strdup_printf("n%u", i);
    n->label = (i < 40) ? g_strdup_printf("Hub Note %u", i) : NULL;
    n->kind = (i % 4 == 0) ? BT_NODE_AI_NOTE : BT_NODE_HUMAN_NOTE;
    n->visible = TRUE;
    n->degree = (i < 25) ? 12u : 3u;
    n->hue = (gdouble)((i * 37) % 360);
    n->radius = (i < 25) ? 10.0 : 5.5;
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

static void free_bench_graph(BtGraph *g)
{
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *n = g_ptr_array_index(g->nodes, i);
    g_free(n->id);
    g_free(n->label);
  }
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
}

static gboolean fallback_hit = FALSE;
static gchar fallback_reason[128];

static void on_fb(const gchar *reason, gpointer ud)
{
  (void)ud;
  fallback_hit = TRUE;
  g_strlcpy(fallback_reason, reason ? reason : "unknown", sizeof(fallback_reason));
}

static void dummy_fill(BtGraphRenderState *out, gpointer ud)
{
  (void)ud;
  out->quality = BT_GRAPH_QUALITY_HIGH;
  out->zoom = 1.0;
  out->dist3d = 2.8;
}

static double bench_gl_case(BtGraphGL *gl, const BtGraph *g, const BtGraphVec3 *pts3d,
                            BtGraphMode mode, BtGraphQuality q, const char *label,
                            guint frames, guint *nonbg_out)
{
  guint n = g->nodes->len;
  guint e = g->edges->len;
  g_autofree guint8 *mask = g_new0(guint8, n);
  guint hub_thresh = bt_graph_hub_degree_threshold(g);

  bt_graph_gl_set_graph(gl, g);
  if (pts3d != NULL)
    bt_graph_gl_set_layout3d(gl, pts3d, n);

  BtGraphRenderState st = {
    .graph = g, .width = 800, .height = 600, .scale = 1,
    .pan_x = 0.0, .pan_y = 0.0, .zoom = 1.0,
    .yaw = 0.35, .pitch = 0.22, .dist3d = 2.7,
    .hover_idx = -1, .selected_idx = 0, .focus_idx = -1,
    .hover_fade = 0.0, .nbr_mask = mask, .nbr_mask_len = n,
    .hub_thresh = hub_thresh, .time_sec = 0.0, .pulse_phase = 0.0,
    .quality = q, .mode = mode, .reduce_motion = FALSE,
    .pulse_enabled = (q == BT_GRAPH_QUALITY_HIGH),
  };

  /* Warmup + initial upload + FBO alloc + atlas build. */
  bt_graph_gl_render_state(gl, &st);
  bt_graph_gl_render_state(gl, &st);
  glFinish();

  gint64 t0 = g_get_monotonic_time();
  for (guint f = 0; f < frames; f++) {
    st.pan_x = (gdouble)(f % 30) * 1.5;
    st.yaw = 0.35 + (gdouble)f * 0.02;
    st.time_sec = (gdouble)f * 0.016;
    st.pulse_phase = fmod((gdouble)f * 0.03, 1.0);
    bt_graph_gl_render_state(gl, &st);
  }
  glFinish();
  double avg_ms = ((double)(g_get_monotonic_time() - t0) / 1000.0) / (double)frames;
  double fps = avg_ms > 0.0 ? 1000.0 / avg_ms : 9999.0;

  if (nonbg_out != NULL) {
    g_autofree unsigned char *px = g_malloc(800u * 600u * 4u);
    glReadPixels(0, 0, 800, 600, GL_RGBA, GL_UNSIGNED_BYTE, px);
    guint diff = 0;
    for (guint i = 0; i < 800u * 600u; i++) {
      if (px[i * 4 + 0] != px[0] || px[i * 4 + 1] != px[1] || px[i * 4 + 2] != px[2])
        diff++;
    }
    *nonbg_out = diff;
  }

  printf("BT_BENCH_GL %-18s nodes=%u edges=%u avg_ms=%.3f fps=%.1f\n",
         label, n, e, avg_ms, fps);
  return avg_ms;
}

int main(void)
{
  if (!gtk_init_check()) {
    printf("BT_BENCH_GL SKIP no display\n");
    return 77;
  }

  GtkWidget *win = gtk_window_new();
  gtk_window_set_default_size(GTK_WINDOW(win), 800, 600);
  BtGraphGL *gl = bt_graph_gl_new(dummy_fill, on_fb, NULL);
  gtk_window_set_child(GTK_WINDOW(win), bt_graph_gl_widget(gl));
  gtk_window_present(GTK_WINDOW(win));

  gint64 deadline = g_get_monotonic_time() + 3000000;
  while (!bt_graph_gl_is_ready(gl) && !fallback_hit && g_get_monotonic_time() < deadline) {
    while (g_main_context_pending(NULL))
      g_main_context_iteration(NULL, FALSE);
    g_usleep(5000);
  }

  if (fallback_hit || !bt_graph_gl_is_ready(gl)) {
    printf("BT_BENCH_GL SKIP fallback=%s\n", fallback_reason[0] ? fallback_reason : "timeout");
    gtk_window_destroy(GTK_WINDOW(win));
    bt_graph_gl_free(gl);
    return 77;
  }

  printf("BT_BENCH_GL info vendor=\"%s\" renderer=\"%s\" version=\"%s\" bloom=%d init_ms=%.2f\n",
         bt_graph_gl_vendor(gl), bt_graph_gl_renderer(gl),
         bt_graph_gl_version(gl), bt_graph_gl_bloom_ready(gl) ? 1 : 0,
         bt_graph_gl_init_ms(gl));

  GtkGLArea *area = GTK_GL_AREA(bt_graph_gl_widget(gl));
  gtk_gl_area_make_current(area);

  GLuint fbo = 0, tex = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 800, 600, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    printf("BT_BENCH_GL SKIP fbo incomplete (0x%x)\n", (unsigned)status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
    gtk_window_destroy(GTK_WINDOW(win));
    bt_graph_gl_free(gl);
    return 77;
  }

  BtGraph *g1k = make_bench_graph(1000, 3000);
  g_autofree BtGraphVec3 *pts1k = g_new0(BtGraphVec3, 1000);
  bt_graph_compute_3d_layout(g1k, pts1k, 1000);

  guint diff_2d = 0, diff_3d = 0;
  double ms_2d_1k_low =
      bench_gl_case(gl, g1k, pts1k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_LOW, "gl_2d_1k_3k_low", 60, NULL);
  double ms_2d_1k_high =
      bench_gl_case(gl, g1k, pts1k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_HIGH, "gl_2d_1k_3k_high", 60, &diff_2d);
  double ms_3d_1k_high =
      bench_gl_case(gl, g1k, pts1k, BT_GRAPH_MODE_3D, BT_GRAPH_QUALITY_HIGH, "gl_3d_1k_3k_high", 60, &diff_3d);

  BtGraph *g3k = make_bench_graph(3000, 9000);
  g_autofree BtGraphVec3 *pts3k = g_new0(BtGraphVec3, 3000);
  bt_graph_compute_3d_layout(g3k, pts3k, 3000);

  double ms_2d_3k_low =
      bench_gl_case(gl, g3k, pts3k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_LOW, "gl_2d_3k_9k_low", 60, NULL);
  double ms_2d_3k_high =
      bench_gl_case(gl, g3k, pts3k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_HIGH, "gl_2d_3k_9k_high", 60, NULL);
  double ms_3d_3k_high =
      bench_gl_case(gl, g3k, pts3k, BT_GRAPH_MODE_3D, BT_GRAPH_QUALITY_HIGH, "gl_3d_3k_9k_high", 60, NULL);

  printf("BT_BENCH_GL pixels_diff_2d=%u pixels_diff_3d=%u\n", diff_2d, diff_3d);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteTextures(1, &tex);
  glDeleteFramebuffers(1, &fbo);

  free_bench_graph(g1k);
  free_bench_graph(g3k);
  gtk_window_destroy(GTK_WINDOW(win));
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
  bt_graph_gl_free(gl);

  gboolean ok = (diff_2d > 500u) && (diff_3d > 500u) &&
                (ms_2d_1k_low < 16.6) && (ms_2d_1k_high < 16.6) &&
                (ms_3d_1k_high < 16.6) && (ms_2d_3k_low < 16.6) &&
                (ms_2d_3k_high < 22.2) && (ms_3d_3k_high < 22.2);
  return ok ? 0 : 1;
}
