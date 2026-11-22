/* bench_graph_gl.c — Headless real-GL benchmark for Brain Graph v2:
 *   - Realizes GtkGLArea, reports GL_VENDOR / GL_RENDERER / GL_VERSION + init_ms.
 *   - Runs each state for 600 frames (override via --frames N) with per-frame
 *     glFinish() timing, computing avg / p95 / max frame time (ms).
 *   - Fails (exit 1) if any case's p95 exceeds the budget in docs/graph-v2.md §4:
 *       1k/3k Low & High (2D & 3D): p95 <= 16.6 ms
 *       3k/9k Low:                  p95 <= 16.6 ms
 *       3k/9k High (2D & 3D):       p95 <= 22.2 ms
 *   - Optional --json <path> (or BT_BENCH_JSON env) writes structured JSON results
 *     (e.g. docs/bench/YYYY-MM-DD.json) for historical comparison.
 *   - Exits 77 (Meson skip) cleanly if no display or GL context is unavailable. */
#include <cjson/cJSON.h>
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_build_stamp.h"
#include "graph.h"
#include "graph_state.h"
#include "ui/graphgl.h"

#define BT_BENCH_DEFAULT_FRAMES 600u
#define BT_BENCH_WIDTH 800
#define BT_BENCH_HEIGHT 600
#define BT_BENCH_INIT_BUDGET_MS 150.0

typedef struct {
  const char *name;
  const char *mode_str;
  const char *quality_str;
  guint nodes;
  guint edges;
  guint frames;
  double avg_ms;
  double p95_ms;
  double max_ms;
  double fps_avg;
  double budget_p95_ms;
  gboolean pass;
} BtBenchCaseResult;

static int cmp_double_asc(const void *a, const void *b)
{
  double da = *(const double *)a;
  double db = *(const double *)b;
  if (da < db)
    return -1;
  if (da > db)
    return 1;
  return 0;
}

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

static BtBenchCaseResult bench_gl_case(BtGraphGL *gl, const BtGraph *g, const BtGraphVec3 *pts3d,
                                       BtGraphMode mode, BtGraphQuality q, const char *label,
                                       guint frames, double budget_p95_ms, guint *nonbg_out)
{
  guint n = g->nodes->len;
  guint e = g->edges->len;
  g_autofree guint8 *mask = g_new0(guint8, n);
  guint hub_thresh = bt_graph_hub_degree_threshold(g);

  bt_graph_gl_set_graph(gl, g);
  if (pts3d != NULL)
    bt_graph_gl_set_layout3d(gl, pts3d, n);

  BtGraphRenderState st = {
    .graph = g, .width = BT_BENCH_WIDTH, .height = BT_BENCH_HEIGHT, .scale = 1,
    .pan_x = 0.0, .pan_y = 0.0, .zoom = 1.0,
    .yaw = 0.35, .pitch = 0.22, .dist3d = 2.7,
    .hover_idx = -1, .selected_idx = 0, .focus_idx = -1,
    .hover_fade = 0.0, .nbr_mask = mask, .nbr_mask_len = n,
    .hub_thresh = hub_thresh, .time_sec = 0.0, .pulse_phase = 0.0,
    .quality = q, .mode = mode, .reduce_motion = FALSE,
    .pulse_enabled = (q == BT_GRAPH_QUALITY_HIGH),
  };

  /* Warmup: initial VBO upload, FBO alloc, and glyph atlas build. */
  bt_graph_gl_render_state(gl, &st);
  bt_graph_gl_render_state(gl, &st);
  glFinish();

  g_autofree double *samples = g_new0(double, frames);
  double sum_ms = 0.0;
  for (guint f = 0; f < frames; f++) {
    st.pan_x = (gdouble)(f % 30) * 1.5;
    st.yaw = 0.35 + (gdouble)f * 0.02;
    st.time_sec = (gdouble)f * 0.016;
    st.pulse_phase = fmod((gdouble)f * 0.03, 1.0);

    gint64 ft0 = g_get_monotonic_time();
    bt_graph_gl_render_state(gl, &st);
    glFinish();
    double dt_ms = (double)(g_get_monotonic_time() - ft0) / 1000.0;
    samples[f] = dt_ms;
    sum_ms += dt_ms;
  }

  qsort(samples, frames, sizeof(double), cmp_double_asc);
  guint p95_idx = (guint)ceil(0.95 * (double)frames);
  if (p95_idx > 0)
    p95_idx -= 1;
  if (p95_idx >= frames)
    p95_idx = frames - 1;

  double avg_ms = sum_ms / (double)frames;
  double p95_ms = samples[p95_idx];
  double max_ms = samples[frames - 1];
  double fps_avg = avg_ms > 0.0 ? 1000.0 / avg_ms : 9999.0;
  gboolean pass = (p95_ms <= budget_p95_ms);

  if (nonbg_out != NULL) {
    g_autofree unsigned char *px = g_malloc((size_t)BT_BENCH_WIDTH * (size_t)BT_BENCH_HEIGHT * 4u);
    glReadPixels(0, 0, BT_BENCH_WIDTH, BT_BENCH_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, px);
    guint diff = 0;
    for (guint i = 0; i < (guint)(BT_BENCH_WIDTH * BT_BENCH_HEIGHT); i++) {
      if (px[i * 4 + 0] != px[0] || px[i * 4 + 1] != px[1] || px[i * 4 + 2] != px[2])
        diff++;
    }
    *nonbg_out = diff;
  }

  printf("BT_BENCH_GL %-18s frames=%u nodes=%u edges=%u avg_ms=%.3f p95_ms=%.3f max_ms=%.3f "
         "budget_p95_ms=%.1f fps=%.1f pass=%d\n",
         label, frames, n, e, avg_ms, p95_ms, max_ms, budget_p95_ms, fps_avg, pass ? 1 : 0);

  BtBenchCaseResult res = {
    .name = label,
    .mode_str = (mode == BT_GRAPH_MODE_3D) ? "3d" : "2d",
    .quality_str = (q == BT_GRAPH_QUALITY_HIGH) ? "high" : "low",
    .nodes = n,
    .edges = e,
    .frames = frames,
    .avg_ms = avg_ms,
    .p95_ms = p95_ms,
    .max_ms = max_ms,
    .fps_avg = fps_avg,
    .budget_p95_ms = budget_p95_ms,
    .pass = pass,
  };
  return res;
}

static gboolean write_bench_json(const char *path, BtGraphGL *gl, guint frames,
                                 const BtBenchCaseResult *cases, guint n_cases,
                                 guint diff_2d, guint diff_3d, gboolean all_pass)
{
  g_autofree gchar *dir = g_path_get_dirname(path);
  if (dir != NULL && dir[0] != '\0' && g_mkdir_with_parents(dir, 0755) != 0) {
    fprintf(stderr, "BT_BENCH_GL failed to create dir: %s\n", dir);
    return FALSE;
  }

  g_autoptr(GDateTime) now_local = g_date_time_new_now_local();
  g_autoptr(GDateTime) now_utc = g_date_time_new_now_utc();
  g_autofree gchar *date_str = g_date_time_format(now_local, "%Y-%m-%d");
  g_autofree gchar *ts_str = g_date_time_format(now_utc, "%Y-%m-%dT%H:%M:%SZ");

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "date", date_str ? date_str : "");
  cJSON_AddStringToObject(root, "timestamp_utc", ts_str ? ts_str : "");
  cJSON_AddStringToObject(root, "build_stamp", BT_BUILD_STAMP);
  cJSON_AddNumberToObject(root, "frames", (double)frames);
  cJSON_AddNumberToObject(root, "viewport_width", BT_BENCH_WIDTH);
  cJSON_AddNumberToObject(root, "viewport_height", BT_BENCH_HEIGHT);
  cJSON_AddStringToObject(root, "gl_vendor", bt_graph_gl_vendor(gl));
  cJSON_AddStringToObject(root, "gl_renderer", bt_graph_gl_renderer(gl));
  cJSON_AddStringToObject(root, "gl_version", bt_graph_gl_version(gl));
  cJSON_AddBoolToObject(root, "bloom_ready", bt_graph_gl_bloom_ready(gl));
  cJSON_AddNumberToObject(root, "init_ms", bt_graph_gl_init_ms(gl));
  cJSON_AddNumberToObject(root, "init_budget_ms", BT_BENCH_INIT_BUDGET_MS);
  cJSON_AddNumberToObject(root, "pixels_diff_2d", (double)diff_2d);
  cJSON_AddNumberToObject(root, "pixels_diff_3d", (double)diff_3d);
  cJSON_AddBoolToObject(root, "all_p95_within_budget", all_pass);

  cJSON *arr = cJSON_AddArrayToObject(root, "cases");
  for (guint i = 0; i < n_cases; i++) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", cases[i].name);
    cJSON_AddStringToObject(item, "mode", cases[i].mode_str);
    cJSON_AddStringToObject(item, "quality", cases[i].quality_str);
    cJSON_AddNumberToObject(item, "nodes", (double)cases[i].nodes);
    cJSON_AddNumberToObject(item, "edges", (double)cases[i].edges);
    cJSON_AddNumberToObject(item, "frames", (double)cases[i].frames);
    cJSON_AddNumberToObject(item, "avg_ms", cases[i].avg_ms);
    cJSON_AddNumberToObject(item, "p95_ms", cases[i].p95_ms);
    cJSON_AddNumberToObject(item, "max_ms", cases[i].max_ms);
    cJSON_AddNumberToObject(item, "fps_avg", cases[i].fps_avg);
    cJSON_AddNumberToObject(item, "budget_p95_ms", cases[i].budget_p95_ms);
    cJSON_AddBoolToObject(item, "pass", cases[i].pass);
    cJSON_AddItemToArray(arr, item);
  }

  char *printed = cJSON_Print(root);
  cJSON_Delete(root);
  if (printed == NULL)
    return FALSE;

  g_autoptr(GError) err = NULL;
  gboolean ok = g_file_set_contents(path, printed, -1, &err);
  if (!ok)
    fprintf(stderr, "BT_BENCH_GL failed writing %s: %s\n", path, err ? err->message : "io error");
  else
    printf("BT_BENCH_GL json=%s\n", path);
  cJSON_free(printed);
  return ok;
}

int main(int argc, char **argv)
{
  const char *json_path = g_getenv("BT_BENCH_JSON");
  guint frames = BT_BENCH_DEFAULT_FRAMES;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      json_path = argv[++i];
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v >= 10 && v <= 10000)
        frames = (guint)v;
    }
  }

  setlocale(LC_NUMERIC, "C");
  if (!gtk_init_check()) {
    printf("BT_BENCH_GL SKIP no display\n");
    return 77;
  }
  /* gtk_init_check() resets locale via setlocale(LC_ALL, ""); restore C numeric
   * formatting so stdout and JSON always use '.' decimal separators. */
  setlocale(LC_NUMERIC, "C");

  GtkWidget *win = gtk_window_new();
  gtk_window_set_default_size(GTK_WINDOW(win), BT_BENCH_WIDTH, BT_BENCH_HEIGHT);
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, BT_BENCH_WIDTH, BT_BENCH_HEIGHT, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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

  BtGraph *g3k = make_bench_graph(3000, 9000);
  g_autofree BtGraphVec3 *pts3k = g_new0(BtGraphVec3, 3000);
  bt_graph_compute_3d_layout(g3k, pts3k, 3000);

  guint diff_2d = 0, diff_3d = 0;
  BtBenchCaseResult results[6];
  results[0] = bench_gl_case(gl, g1k, pts1k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_LOW,
                             "gl_2d_1k_3k_low", frames, 16.6, NULL);
  results[1] = bench_gl_case(gl, g1k, pts1k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_HIGH,
                             "gl_2d_1k_3k_high", frames, 16.6, &diff_2d);
  results[2] = bench_gl_case(gl, g1k, pts1k, BT_GRAPH_MODE_3D, BT_GRAPH_QUALITY_HIGH,
                             "gl_3d_1k_3k_high", frames, 16.6, &diff_3d);
  results[3] = bench_gl_case(gl, g3k, pts3k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_LOW,
                             "gl_2d_3k_9k_low", frames, 16.6, NULL);
  results[4] = bench_gl_case(gl, g3k, pts3k, BT_GRAPH_MODE_2D, BT_GRAPH_QUALITY_HIGH,
                             "gl_2d_3k_9k_high", frames, 22.2, NULL);
  results[5] = bench_gl_case(gl, g3k, pts3k, BT_GRAPH_MODE_3D, BT_GRAPH_QUALITY_HIGH,
                             "gl_3d_3k_9k_high", frames, 22.2, NULL);

  printf("BT_BENCH_GL pixels_diff_2d=%u pixels_diff_3d=%u\n", diff_2d, diff_3d);

  gboolean all_p95_pass = TRUE;
  for (guint i = 0; i < G_N_ELEMENTS(results); i++) {
    if (!results[i].pass)
      all_p95_pass = FALSE;
  }
  gboolean json_ok = TRUE;
  if (json_path != NULL && json_path[0] != '\0')
    json_ok = write_bench_json(json_path, gl, frames, results, G_N_ELEMENTS(results),
                               diff_2d, diff_3d, all_p95_pass);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteTextures(1, &tex);
  glDeleteFramebuffers(1, &fbo);

  free_bench_graph(g1k);
  free_bench_graph(g3k);
  gtk_window_destroy(GTK_WINDOW(win));
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
  bt_graph_gl_free(gl);

  gboolean ok = (diff_2d > 500u) && (diff_3d > 500u) && all_p95_pass && json_ok;
  return ok ? 0 : 1;
}
