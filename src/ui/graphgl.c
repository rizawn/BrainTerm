/* graphgl.c — OpenGL 3.3 Core / GLES 3.0 Brain Graph v2 backend (Steps 2–3):
 *   - Instanced quads for glowing nodes (2D & 3D orbit projection).
 *   - Instanced curved/straight additive edges (+ optional selected-node pulse).
 *   - GPU-animated drifting dust sprites (zero CPU per-frame upload).
 *   - Half-resolution two-pass Gaussian bloom FBO (High quality, auto-downgrade if FBO fails).
 *   - Cached Pango RGBA label atlas rendered crisp after bloom composite.
 *   - Zero per-frame heap allocations; automatic Cairo fallback on any GL/shader error. */
#include "ui/graphgl.h"
#include "graph_palette.h"
#include "theme.h"

#include <cairo.h>
#include <epoxy/gl.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BT_GL_DUST_MAX    128u
#define BT_GL_LABEL_CAP   128u
#define BT_GL_ATLAS_MAX_W 2048
#define BT_GL_ATLAS_MAX_H 1024

typedef struct {
  float x0, y0, vx, vy;
  float radius, alpha;
} DustInst;

typedef struct {
  float ax, ay, bx, by;
  float curve_sign, src_idx, dst_idx, kind;
} EdgeInst;

typedef struct {
  float ax, ay, az;
  float bx, by, bz;
} Edge3DInst;

typedef struct {
  float x, y, radius, kind;
  float r, g, b, vis;
} NodeInst;

typedef struct {
  float x, y, w, h;
  float u0, v0, u1, v1;
  float alpha, pill;
} LabelInst;

typedef struct {
  gboolean valid;
  float u0, v0, u1, v1;
  float w_log, h_log;
} AtlasSlot;

struct BtGraphGL {
  GtkWidget *area; /* GtkGLArea */
  BtGraphGLStateFillFn fill_state;
  BtGraphGLFallbackFn on_fallback;
  gpointer user_data;

  const BtGraph *graph; /* borrowed from BtGraphView */
  BtGraphVec3 *pts3d;   /* owned snapshot copied on bt_graph_gl_set_layout3d */
  guint pts3d_count;

  gboolean gl_ready;
  gboolean gl_failed;
  gboolean bloom_supported;
  gboolean bloom_ready;
  gboolean graph_dirty;
  gboolean layout3d_dirty;
  gboolean state_dirty;
  gboolean atlas_dirty;
  gint atlas_scale;

  gchar vendor_str[128];
  gchar renderer_str[128];
  gchar version_str[128];
  gdouble init_ms;

  /* Core shader programs */
  GLuint prog_bg;
  GLuint prog_dust;
  GLuint prog_edge;
  GLuint prog_node;
  GLuint prog_label;

  /* Bloom post-process programs */
  GLuint prog_extract;
  GLuint prog_blur;
  GLuint prog_composite;

  /* Cached uniform locations */
  GLint u_bg_col, u_bg_vig;
  GLint u_dust_vp, u_dust_time, u_dust_scale, u_dust_col, u_dust_int;
  GLint u_edge_vp, u_edge_pan, u_edge_zoom, u_edge_scale, u_edge_bt;
  GLint u_edge_foc, u_edge_sel, u_edge_hf, u_edge_segs, u_edge_curved;
  GLint u_edge_ba, u_edge_hw, u_edge_dim_col, u_edge_act_col;
  GLint u_edge_mode3d, u_edge_mvp, u_edge_pulse_en, u_edge_pulse_ph, u_edge_show_links;
  GLint u_node_vp, u_node_pan, u_node_zoom, u_node_scale, u_node_bt;
  GLint u_node_has_foc, u_node_hf, u_node_hq, u_node_glow_col, u_node_s2_col;
  GLint u_node_mode3d, u_node_mvp, u_node_mouse, u_node_spread, u_node_dim_alpha;
  GLint u_lbl_vp, u_lbl_atlas, u_lbl_tp, u_lbl_ts, u_lbl_pbg, u_lbl_bg;
  GLint u_ext_scene, u_ext_thresh;
  GLint u_blur_tex, u_blur_dir;
  GLint u_comp_scene, u_comp_bloom, u_comp_str;

  /* VAOs & VBOs */
  GLuint vao_bg;
  GLuint vao_dust, vbo_dust;
  GLuint vao_edge, vbo_edge, vbo_edge3d;
  GLuint vao_node, vbo_node_static, vbo_node_state, vbo_node3d;
  GLuint vao_label, vbo_label;
  GLuint tex_atlas;

  /* Bloom FBOs (full-res scene + half-res ping-pong blur) */
  GLuint scene_fbo, scene_tex;
  GLuint bloom_fbo[2], bloom_tex[2];
  int fbo_w, fbo_h;

  guint edge_count;
  guint node_count;
  guint node_state_cap;
  float *node_state_scratch;

  AtlasSlot *atlas_slots;
  guint atlas_slot_count;
  LabelInst label_scratch[BT_GL_LABEL_CAP];
  BtGraphLabelCand *lbl_cands;
  BtGraphLabelCand lbl_placed[BT_GL_LABEL_CAP];
  BtGraphCoreObstacle *obs_scratch;
  guint lbl_scratch_cap;
  guint last_label_count;
  guint last_overlap_count;
};

/* ----------------------------------------------------- shader helpers -- */

static gchar *load_shader_resource(const gchar *name)
{
  gchar *path = g_strdup_printf("/local/brainterm/shaders/%s", name);
  GBytes *bytes = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  g_free(path);
  if (bytes == NULL)
    return NULL;
  gsize sz = 0;
  const gchar *data = g_bytes_get_data(bytes, &sz);
  gchar *out = g_strndup(data, sz);
  g_bytes_unref(bytes);
  return out;
}

static GLuint compile_stage(GLenum type, const gchar *res_name, gboolean is_es)
{
  gchar *body = load_shader_resource(res_name);
  if (body == NULL) {
    g_warning("graphgl: missing shader resource %s", res_name);
    return 0;
  }
  const gchar *header = is_es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                              : "#version 330 core\n";
  const GLchar *srcs[2] = {header, body};
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 2, srcs, NULL);
  glCompileShader(sh);
  g_free(body);

  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
    g_warning("graphgl: compile failed (%s): %s", res_name, log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static GLuint link_program(const gchar *vs_name, const gchar *fs_name, gboolean is_es)
{
  GLuint vs = compile_stage(GL_VERTEX_SHADER, vs_name, is_es);
  if (vs == 0)
    return 0;
  GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fs_name, is_es);
  if (fs == 0) {
    glDeleteShader(vs);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
    g_warning("graphgl: link failed (%s + %s): %s", vs_name, fs_name, log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

static guint32 kind_rgb(BtNodeKind k)
{
  switch (k) {
  case BT_NODE_HUMAN_NOTE:  return 0xBFD7FFu; /* pale star-white-blue instead of harsh white */
  case BT_NODE_AI_NOTE:     return BT_COL_NODE_AI;
  case BT_NODE_TAG:         return BT_COL_NODE_TAG;
  case BT_NODE_FOLDER:      return 0xFFC857u; /* warm core gold default until Phase 2 palette */
  case BT_NODE_IMPORT_ROOT: return BT_COL_NODE_FOLDER;
  case BT_NODE_SESSION:     return BT_COL_TEXT_SECONDARY;
  case BT_NODE_GHOST:
  default:                  return BT_COL_TEXT_MUTED;
  }
}

static guint32 lcg_u32(guint32 *s)
{
  *s = (*s) * 1664525u + 1013904223u;
  return *s;
}

/* ----------------------------------------------------- FBO & GL cleanup -- */

static void destroy_bloom_fbos(BtGraphGL *gl)
{
  if (gl->scene_fbo)    { glDeleteFramebuffers(1, &gl->scene_fbo);    gl->scene_fbo = 0; }
  if (gl->scene_tex)    { glDeleteTextures(1, &gl->scene_tex);        gl->scene_tex = 0; }
  for (int i = 0; i < 2; i++) {
    if (gl->bloom_fbo[i]) { glDeleteFramebuffers(1, &gl->bloom_fbo[i]); gl->bloom_fbo[i] = 0; }
    if (gl->bloom_tex[i]) { glDeleteTextures(1, &gl->bloom_tex[i]);     gl->bloom_tex[i] = 0; }
  }
  gl->fbo_w = 0;
  gl->fbo_h = 0;
  gl->bloom_ready = FALSE;
}

static gboolean alloc_color_fbo(GLuint *fbo_out, GLuint *tex_out, int w, int h)
{
  GLuint fbo = 0, tex = 0;
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return FALSE;
  }
  *fbo_out = fbo;
  *tex_out = tex;
  return TRUE;
}

static gboolean ensure_bloom_fbos(BtGraphGL *gl, int fb_w, int fb_h, GLint restore_fbo)
{
  if (!gl->bloom_supported || fb_w < 4 || fb_h < 4)
    return FALSE;
  if (gl->bloom_ready && gl->fbo_w == fb_w && gl->fbo_h == fb_h)
    return TRUE;

  destroy_bloom_fbos(gl);
  int hw = MAX(fb_w / 2, 2);
  int hh = MAX(fb_h / 2, 2);
  gboolean ok = alloc_color_fbo(&gl->scene_fbo, &gl->scene_tex, fb_w, fb_h) &&
                alloc_color_fbo(&gl->bloom_fbo[0], &gl->bloom_tex[0], hw, hh) &&
                alloc_color_fbo(&gl->bloom_fbo[1], &gl->bloom_tex[1], hw, hh);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)restore_fbo);
  if (!ok) {
    destroy_bloom_fbos(gl);
    gl->bloom_supported = FALSE;
    return FALSE;
  }
  gl->fbo_w = fb_w;
  gl->fbo_h = fb_h;
  gl->bloom_ready = TRUE;
  return TRUE;
}

static void destroy_gl_objects(BtGraphGL *gl)
{
  destroy_bloom_fbos(gl);
  if (gl->prog_bg)        { glDeleteProgram(gl->prog_bg);        gl->prog_bg = 0; }
  if (gl->prog_dust)      { glDeleteProgram(gl->prog_dust);      gl->prog_dust = 0; }
  if (gl->prog_edge)      { glDeleteProgram(gl->prog_edge);      gl->prog_edge = 0; }
  if (gl->prog_node)      { glDeleteProgram(gl->prog_node);      gl->prog_node = 0; }
  if (gl->prog_label)     { glDeleteProgram(gl->prog_label);     gl->prog_label = 0; }
  if (gl->prog_extract)   { glDeleteProgram(gl->prog_extract);   gl->prog_extract = 0; }
  if (gl->prog_blur)      { glDeleteProgram(gl->prog_blur);      gl->prog_blur = 0; }
  if (gl->prog_composite) { glDeleteProgram(gl->prog_composite); gl->prog_composite = 0; }

  if (gl->vao_bg)    { glDeleteVertexArrays(1, &gl->vao_bg);    gl->vao_bg = 0; }
  if (gl->vao_dust)  { glDeleteVertexArrays(1, &gl->vao_dust);  gl->vao_dust = 0; }
  if (gl->vao_edge)  { glDeleteVertexArrays(1, &gl->vao_edge);  gl->vao_edge = 0; }
  if (gl->vao_node)  { glDeleteVertexArrays(1, &gl->vao_node);  gl->vao_node = 0; }
  if (gl->vao_label) { glDeleteVertexArrays(1, &gl->vao_label); gl->vao_label = 0; }

  if (gl->vbo_dust)        { glDeleteBuffers(1, &gl->vbo_dust);        gl->vbo_dust = 0; }
  if (gl->vbo_edge)        { glDeleteBuffers(1, &gl->vbo_edge);        gl->vbo_edge = 0; }
  if (gl->vbo_edge3d)      { glDeleteBuffers(1, &gl->vbo_edge3d);      gl->vbo_edge3d = 0; }
  if (gl->vbo_node_static) { glDeleteBuffers(1, &gl->vbo_node_static); gl->vbo_node_static = 0; }
  if (gl->vbo_node_state)  { glDeleteBuffers(1, &gl->vbo_node_state);  gl->vbo_node_state = 0; }
  if (gl->vbo_node3d)      { glDeleteBuffers(1, &gl->vbo_node3d);      gl->vbo_node3d = 0; }
  if (gl->vbo_label)       { glDeleteBuffers(1, &gl->vbo_label);       gl->vbo_label = 0; }
  if (gl->tex_atlas)       { glDeleteTextures(1, &gl->tex_atlas);      gl->tex_atlas = 0; }
  gl->gl_ready = FALSE;
}

static void init_dust_vbo(BtGraphGL *gl)
{
  DustInst pts[BT_GL_DUST_MAX];
  guint32 seed = 0xB7A1E29Du;
  for (guint i = 0; i < BT_GL_DUST_MAX; i++) {
    float u0 = (float)(lcg_u32(&seed) & 0xFFFFu) / 65535.0f;
    float u1 = (float)(lcg_u32(&seed) & 0xFFFFu) / 65535.0f;
    float u2 = (float)(lcg_u32(&seed) & 0xFFFFu) / 65535.0f;
    float u3 = (float)(lcg_u32(&seed) & 0xFFFFu) / 65535.0f;
    float u4 = (float)(lcg_u32(&seed) & 0xFFFFu) / 65535.0f;
    pts[i].x0 = u0;
    pts[i].y0 = u1;
    pts[i].vx = (u2 - 0.5f) * 0.012f;
    pts[i].vy = -0.004f - u3 * 0.009f;
    pts[i].radius = 0.9f + u4 * 1.5f;
    pts[i].alpha = 0.09f + u4 * 0.18f;
  }

  glGenVertexArrays(1, &gl->vao_dust);
  glGenBuffers(1, &gl->vbo_dust);
  glBindVertexArray(gl->vao_dust);
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_dust);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(pts), pts, GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(DustInst),
                        (const void *)(uintptr_t)offsetof(DustInst, x0));
  glVertexAttribDivisor(0, 1);

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(DustInst),
                        (const void *)(uintptr_t)offsetof(DustInst, radius));
  glVertexAttribDivisor(1, 1);
  glBindVertexArray(0);
}

static void init_dynamic_vaos(BtGraphGL *gl)
{
  glGenVertexArrays(1, &gl->vao_bg);

  /* Edges VAO: loc 0..1 = 2D static, loc 2..3 = 3D static */
  glGenVertexArrays(1, &gl->vao_edge);
  glGenBuffers(1, &gl->vbo_edge);
  glGenBuffers(1, &gl->vbo_edge3d);
  glBindVertexArray(gl->vao_edge);
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_edge);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(EdgeInst),
                        (const void *)(uintptr_t)offsetof(EdgeInst, ax));
  glVertexAttribDivisor(0, 1);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(EdgeInst),
                        (const void *)(uintptr_t)offsetof(EdgeInst, curve_sign));
  glVertexAttribDivisor(1, 1);

  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_edge3d);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(Edge3DInst),
                        (const void *)(uintptr_t)offsetof(Edge3DInst, ax));
  glVertexAttribDivisor(2, 1);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(Edge3DInst),
                        (const void *)(uintptr_t)offsetof(Edge3DInst, bx));
  glVertexAttribDivisor(3, 1);

  /* Nodes VAO: loc 0..1 = 2D static, loc 2 = dynamic state, loc 3 = 3D static */
  glGenVertexArrays(1, &gl->vao_node);
  glGenBuffers(1, &gl->vbo_node_static);
  glGenBuffers(1, &gl->vbo_node_state);
  glGenBuffers(1, &gl->vbo_node3d);
  glBindVertexArray(gl->vao_node);
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node_static);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(NodeInst),
                        (const void *)(uintptr_t)offsetof(NodeInst, x));
  glVertexAttribDivisor(0, 1);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(NodeInst),
                        (const void *)(uintptr_t)offsetof(NodeInst, r));
  glVertexAttribDivisor(1, 1);

  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node_state);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(float), (const void *)0);
  glVertexAttribDivisor(2, 1);

  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node3d);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(BtGraphVec3), (const void *)0);
  glVertexAttribDivisor(3, 1);

  /* Labels VAO + pre-allocated VBO */
  glGenVertexArrays(1, &gl->vao_label);
  glGenBuffers(1, &gl->vbo_label);
  glBindVertexArray(gl->vao_label);
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_label);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(BT_GL_LABEL_CAP * sizeof(LabelInst)), NULL,
               GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(LabelInst),
                        (const void *)(uintptr_t)offsetof(LabelInst, x));
  glVertexAttribDivisor(0, 1);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(LabelInst),
                        (const void *)(uintptr_t)offsetof(LabelInst, u0));
  glVertexAttribDivisor(1, 1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(LabelInst),
                        (const void *)(uintptr_t)offsetof(LabelInst, alpha));
  glVertexAttribDivisor(2, 1);

  glBindVertexArray(0);
  glGenTextures(1, &gl->tex_atlas);
}

/* ------------------------------------------- graph, 3D & atlas uploads -- */

static void rebuild_label_atlas(BtGraphGL *gl, gint scale)
{
  gl->atlas_dirty = FALSE;
  gl->atlas_scale = MAX(scale, 1);
  g_clear_pointer(&gl->atlas_slots, g_free);
  gl->atlas_slot_count = 0;

  if (gl->graph == NULL || gl->graph->nodes == NULL || gl->graph->nodes->len == 0)
    return;

  guint n = gl->graph->nodes->len;
  gl->atlas_slots = g_new0(AtlasSlot, n);
  gl->atlas_slot_count = n;

  int aw = BT_GL_ATLAS_MAX_W;
  int ah = BT_GL_ATLAS_MAX_H;
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, aw, ah);
  cairo_t *cr = cairo_create(surf);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  PangoLayout *pl = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = pango_font_description_from_string("Inter, Segoe UI, Sans 10");
  pango_font_description_set_size(fd, (gint)(10.5 * PANGO_SCALE * (gdouble)gl->atlas_scale));
  pango_layout_set_font_description(pl, fd);
  pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(pl, 220 * gl->atlas_scale * PANGO_SCALE);

  int pad = 4 * gl->atlas_scale;
  int cx = pad, cy = pad, row_h = 0;
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);

  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gl->graph->nodes, i);
    if (!nd->visible || nd->label == NULL || nd->label[0] == '\0')
      continue;
    g_autofree gchar *badge_lbl = NULL;
    const gchar *txt = nd->label;
    if (nd->kind == BT_NODE_FOLDER && nd->collapsed && nd->child_count > 0) {
      badge_lbl = g_strdup_printf("%s (+%u)", nd->label, nd->child_count);
      txt = badge_lbl;
    }
    pango_layout_set_text(pl, txt, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(pl, &tw, &th);
    if (tw <= 0 || th <= 0)
      continue;
    int bw = tw + pad * 2;
    int bh = th + pad;
    if (cx + bw + 2 > aw) {
      cx = pad;
      cy += row_h + 2;
      row_h = 0;
    }
    if (cy + bh + 2 > ah)
      break;

    cairo_move_to(cr, (gdouble)(cx + pad), (gdouble)(cy + pad / 2));
    pango_cairo_show_layout(cr, pl);

    gl->atlas_slots[i].valid = TRUE;
    gl->atlas_slots[i].u0 = (float)cx / (float)aw;
    gl->atlas_slots[i].v0 = (float)cy / (float)ah;
    gl->atlas_slots[i].u1 = (float)(cx + bw) / (float)aw;
    gl->atlas_slots[i].v1 = (float)(cy + bh) / (float)ah;
    gl->atlas_slots[i].w_log = (float)bw / (float)gl->atlas_scale;
    gl->atlas_slots[i].h_log = (float)bh / (float)gl->atlas_scale;

    cx += bw + 2;
    if (bh > row_h)
      row_h = bh;
  }

  pango_font_description_free(fd);
  g_object_unref(pl);
  cairo_destroy(cr);
  cairo_surface_flush(surf);

  int used_h = MIN(MAX(cy + row_h + 4, 64), ah);
  unsigned char *src = cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf);
  g_autofree unsigned char *rgba = g_malloc((gsize)aw * (gsize)used_h * 4u);
  for (int y = 0; y < used_h; y++) {
    const unsigned char *srow = src + y * stride;
    unsigned char *drow = rgba + (gsize)y * (gsize)aw * 4u;
    for (int x = 0; x < aw; x++) {
      drow[x * 4 + 0] = srow[x * 4 + 2];
      drow[x * 4 + 1] = srow[x * 4 + 1];
      drow[x * 4 + 2] = srow[x * 4 + 0];
      drow[x * 4 + 3] = srow[x * 4 + 3];
    }
  }
  float v_scale = (float)ah / (float)used_h;
  for (guint i = 0; i < n; i++) {
    if (gl->atlas_slots[i].valid) {
      gl->atlas_slots[i].v0 *= v_scale;
      gl->atlas_slots[i].v1 *= v_scale;
    }
  }

  glBindTexture(GL_TEXTURE_2D, gl->tex_atlas);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, aw, used_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  cairo_surface_destroy(surf);
}

static void upload_layout3d_buffers(BtGraphGL *gl)
{
  gl->layout3d_dirty = FALSE;
  const BtGraph *g = gl->graph;
  if (g == NULL || gl->node_count == 0)
    return;

  /* Upload per-node 3D positions (zero-filled fallback if worker hasn't swapped yet). */
  g_autofree BtGraphVec3 *n3 = g_new0(BtGraphVec3, gl->node_count);
  if (gl->pts3d != NULL && gl->pts3d_count > 0) {
    guint copy_n = MIN(gl->pts3d_count, gl->node_count);
    memcpy(n3, gl->pts3d, copy_n * sizeof(BtGraphVec3));
  }
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node3d);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gl->node_count * sizeof(BtGraphVec3)), n3,
               GL_STATIC_DRAW);

  if (gl->edge_count > 0 && g->edges != NULL) {
    g_autofree Edge3DInst *e3 = g_new0(Edge3DInst, gl->edge_count);
    guint out_e = 0;
    for (guint i = 0; i < g->edges->len && out_e < gl->edge_count; i++) {
      const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
      if (e->src_idx >= gl->node_count || e->dst_idx >= gl->node_count)
        continue;
      const BtGraphNode *a = g_ptr_array_index(g->nodes, e->src_idx);
      const BtGraphNode *b = g_ptr_array_index(g->nodes, e->dst_idx);
      if (!a->visible || !b->visible)
        continue;
      e3[out_e].ax = n3[e->src_idx].x;
      e3[out_e].ay = n3[e->src_idx].y;
      e3[out_e].az = n3[e->src_idx].z;
      e3[out_e].bx = n3[e->dst_idx].x;
      e3[out_e].by = n3[e->dst_idx].y;
      e3[out_e].bz = n3[e->dst_idx].z;
      out_e++;
    }
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_edge3d);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gl->edge_count * sizeof(Edge3DInst)), e3,
                 GL_STATIC_DRAW);
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void upload_graph_buffers(BtGraphGL *gl)
{
  gl->graph_dirty = FALSE;
  const BtGraph *g = gl->graph;
  if (g == NULL || g->nodes == NULL || g->nodes->len == 0) {
    gl->node_count = 0;
    gl->edge_count = 0;
    return;
  }

  guint nn = g->nodes->len;
  g_autofree NodeInst *nbuf = g_new(NodeInst, nn);
  for (guint i = 0; i < nn; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    float cr = 0.0f, cg = 0.0f, cb = 0.0f;
    guint32 rgb = (nd->color_rgb != 0u) ? nd->color_rgb : kind_rgb(nd->kind);
    bt_theme_unpack_f(rgb, &cr, &cg, &cb);
    nbuf[i].x = (float)nd->x;
    nbuf[i].y = (float)nd->y;
    nbuf[i].radius = (float)nd->radius;
    nbuf[i].kind = (float)nd->kind;
    nbuf[i].r = cr;
    nbuf[i].g = cg;
    nbuf[i].b = cb;
    nbuf[i].vis = nd->visible ? 1.0f : 0.0f;
  }
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node_static);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nn * sizeof(NodeInst)), nbuf, GL_STATIC_DRAW);

  if (nn > gl->node_state_cap) {
    g_free(gl->node_state_scratch);
    gl->node_state_scratch = g_new0(float, nn);
    gl->node_state_cap = nn;
  } else {
    memset(gl->node_state_scratch, 0, nn * sizeof(float));
  }
  if (nn > gl->lbl_scratch_cap) {
    g_free(gl->lbl_cands);
    g_free(gl->obs_scratch);
    gl->lbl_cands = g_new0(BtGraphLabelCand, nn);
    gl->obs_scratch = g_new0(BtGraphCoreObstacle, nn);
    gl->lbl_scratch_cap = nn;
  }
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node_state);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nn * sizeof(float)), gl->node_state_scratch,
               GL_DYNAMIC_DRAW);
  gl->node_count = nn;

  guint ne = (g->edges != NULL) ? g->edges->len : 0u;
  if (ne > 0u) {
    g_autofree EdgeInst *ebuf = g_new(EdgeInst, ne);
    guint out_e = 0;
    for (guint i = 0; i < ne; i++) {
      const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
      if (e->src_idx >= nn || e->dst_idx >= nn)
        continue;
      const BtGraphNode *a = g_ptr_array_index(g->nodes, e->src_idx);
      const BtGraphNode *b = g_ptr_array_index(g->nodes, e->dst_idx);
      if (!a->visible || !b->visible)
        continue;
      ebuf[out_e].ax = (float)a->x;
      ebuf[out_e].ay = (float)a->y;
      ebuf[out_e].bx = (float)b->x;
      ebuf[out_e].by = (float)b->y;
      ebuf[out_e].curve_sign = ((e->src_idx ^ e->dst_idx) & 1u) ? 1.0f : -1.0f;
      ebuf[out_e].src_idx = (float)e->src_idx;
      ebuf[out_e].dst_idx = (float)e->dst_idx;
      ebuf[out_e].kind = (float)e->kind;
      out_e++;
    }
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_edge);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(out_e * sizeof(EdgeInst)), ebuf, GL_STATIC_DRAW);
    gl->edge_count = out_e;
  } else {
    gl->edge_count = 0;
  }

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  gl->state_dirty = TRUE;
  gl->atlas_dirty = TRUE;
  gl->layout3d_dirty = TRUE;
}

static void upload_node_states(BtGraphGL *gl, const BtGraphRenderState *st)
{
  gl->state_dirty = FALSE;
  if (gl->node_count == 0 || gl->node_state_scratch == NULL)
    return;

  for (guint i = 0; i < gl->node_count; i++) {
    float v = 0.0f;
    if ((gint)i == st->focus_idx)
      v = 4.0f;
    else if ((gint)i == st->selected_idx)
      v = 3.0f;
    else if (st->nbr_mask != NULL && i < st->nbr_mask_len)
      v = (float)st->nbr_mask[i];
    gl->node_state_scratch[i] = v;
  }
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_node_state);
  glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(gl->node_count * sizeof(float)),
                  gl->node_state_scratch);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ---------------------------------------------------------- rendering -- */

void bt_graph_gl_render_state(BtGraphGL *gl, const BtGraphRenderState *st)
{
  if (gl == NULL || !gl->gl_ready || st == NULL || st->width <= 0 || st->height <= 0)
    return;

  if (gl->graph != st->graph) {
    gl->graph = st->graph;
    gl->graph_dirty = TRUE;
  }
  if (gl->graph_dirty)
    upload_graph_buffers(gl);
  if (gl->layout3d_dirty)
    upload_layout3d_buffers(gl);

  gint scale = MAX(st->scale, 1);
  if (gl->atlas_dirty || gl->atlas_scale != scale)
    rebuild_label_atlas(gl, scale);
  if (gl->state_dirty)
    upload_node_states(gl, st);

  int fb_w = st->width * scale;
  int fb_h = st->height * scale;
  gboolean high = (st->quality == BT_GRAPH_QUALITY_HIGH);
  gboolean mode_3d = (st->mode == BT_GRAPH_MODE_3D && gl->pts3d != NULL && gl->pts3d_count > 0);

  GLint target_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target_fbo);

  gboolean use_bloom = high && ensure_bloom_fbos(gl, fb_w, fb_h, target_fbo);
  if (use_bloom)
    glBindFramebuffer(GL_FRAMEBUFFER, gl->scene_fbo);

  glViewport(0, 0, fb_w, fb_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  float r, g, b;

  /* Pass 1: deep navy space canvas gradient (#05070D -> #02030A) */
  glDisable(GL_BLEND);
  glUseProgram(gl->prog_bg);
  bt_theme_unpack_f(BT_GALAXY_BG_TOP, &r, &g, &b);
  glUniform3f(gl->u_bg_col, r, g, b);
  bt_theme_unpack_f(BT_GALAXY_BG_BOTTOM, &r, &g, &b);
  glUniform3f(gl->u_bg_vig, r, g, b);
  glBindVertexArray(gl->vao_bg);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  float vp_w = (float)st->width;
  float vp_h = (float)st->height;

  /* Pass 2: GPU drifting dust sprites (additive) */
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glUseProgram(gl->prog_dust);
  glUniform2f(gl->u_dust_vp, vp_w, vp_h);
  glUniform1f(gl->u_dust_time, st->reduce_motion ? 0.0f : (float)st->time_sec);
  glUniform1f(gl->u_dust_scale, 1.0f);
  bt_theme_unpack_f(BT_COL_GRAPH_DUST, &r, &g, &b);
  glUniform3f(gl->u_dust_col, r, g, b);
  glUniform1f(gl->u_dust_int, high ? 0.95f : 0.45f);
  glBindVertexArray(gl->vao_dust);
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, high ? (GLsizei)BT_GL_DUST_MAX : 32);

  float mvp[16];
  if (mode_3d) {
    bt_graph_build_orbit_mvp(st->yaw, st->pitch, MAX(st->dist3d, 1.2),
                             (gdouble)st->width / MAX((gdouble)st->height, 1.0), mvp);
  } else {
    memset(mvp, 0, sizeof(mvp));
    mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f;
  }

  float bt = 1.0f;
  if (st->graph != NULL && st->graph->nodes->len > 0) {
    const BtGraphNode *n0 = g_ptr_array_index(st->graph->nodes, 0);
    if (n0->birth_t < 0.999)
      bt = (float)n0->birth_t;
  }

  gint focus_idx = (st->hover_idx >= 0)
                       ? st->hover_idx
                       : ((st->focus_idx >= 0) ? st->focus_idx : st->selected_idx);
  gboolean has_focus = (focus_idx >= 0 && (guint)focus_idx < gl->node_count);
  float hf = has_focus ? (float)CLAMP(st->hover_fade, 0.0, 1.0) : 0.0f;
  if (st->hover_idx < 0 && (st->focus_idx >= 0 || st->selected_idx >= 0))
    hf = 1.0f;

  if (st->graph != NULL && gl->node_count > 0) {
    /* Pass 3: instanced edges (normal alpha in 3D/dense to prevent white blowout) */
    if (gl->edge_count > 0) {
      int segs = high ? 6 : 1;
      glBlendFunc(GL_SRC_ALPHA, mode_3d ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
      glUseProgram(gl->prog_edge);
      glUniform2f(gl->u_edge_vp, vp_w, vp_h);
      glUniform2f(gl->u_edge_pan, (float)st->pan_x, (float)st->pan_y);
      glUniform1f(gl->u_edge_zoom, (float)st->zoom);
      glUniform1f(gl->u_edge_scale, 1.0f);
      glUniform1f(gl->u_edge_bt, bt);
      glUniform1i(gl->u_edge_foc, focus_idx);
      glUniform1i(gl->u_edge_sel, st->selected_idx);
      glUniform1f(gl->u_edge_hf, hf);
      glUniform1i(gl->u_edge_segs, segs);
      glUniform1i(gl->u_edge_curved, high ? 1 : 0);
      glUniform1f(gl->u_edge_ba, high ? (mode_3d ? 0.16f : 0.20f) : 0.14f);
      glUniform1f(gl->u_edge_hw, high ? 0.85f : 0.70f);
      bt_theme_unpack_f(BT_COL_GRAPH_EDGE_DIM, &r, &g, &b);
      glUniform3f(gl->u_edge_dim_col, r, g, b);
      bt_theme_unpack_f(BT_COL_GRAPH_EDGE_ACTIVE, &r, &g, &b);
      glUniform3f(gl->u_edge_act_col, r, g, b);
      glUniform1i(gl->u_edge_mode3d, mode_3d ? 1 : 0);
      glUniformMatrix4fv(gl->u_edge_mvp, 1, GL_FALSE, mvp);
      glUniform1i(gl->u_edge_pulse_en, (st->pulse_enabled && !st->reduce_motion) ? 1 : 0);
      glUniform1f(gl->u_edge_pulse_ph, (float)st->pulse_phase);
      glUniform1i(gl->u_edge_show_links, st->show_links ? 1 : 0);
      glBindVertexArray(gl->vao_edge);
      glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, (segs + 1) * 2, (GLsizei)gl->edge_count);
    }

    /* Pass 4: instanced glowing nodes (normal alpha blending, clamped brightness) */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(gl->prog_node);
    glUniform2f(gl->u_node_vp, vp_w, vp_h);
    glUniform2f(gl->u_node_pan, (float)st->pan_x, (float)st->pan_y);
    glUniform1f(gl->u_node_zoom, (float)st->zoom);
    glUniform1f(gl->u_node_scale, 1.0f);
    glUniform1f(gl->u_node_bt, bt);
    glUniform1f(gl->u_node_has_foc, has_focus ? 1.0f : 0.0f);
    glUniform1f(gl->u_node_hf, hf);
    glUniform1i(gl->u_node_hq, high ? 1 : 0);
    bt_theme_unpack_f(BT_COL_GRAPH_NODE_GLOW, &r, &g, &b);
    glUniform3f(gl->u_node_glow_col, r, g, b);
    bt_theme_unpack_f(BT_COL_SURFACE_2, &r, &g, &b);
    glUniform3f(gl->u_node_s2_col, r, g, b);
    glUniform1i(gl->u_node_mode3d, mode_3d ? 1 : 0);
    glUniformMatrix4fv(gl->u_node_mvp, 1, GL_FALSE, mvp);
    glUniform2f(gl->u_node_mouse, (float)st->mouse_x, (float)st->mouse_y);
    glUniform1f(gl->u_node_spread, (mode_3d && !st->reduce_motion) ? (float)st->spread_str : 0.0f);
    glUniform1f(gl->u_node_dim_alpha, st->focus_isolated ? 0.08f : 0.25f);
    glBindVertexArray(gl->vao_node);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)gl->node_count);
  }

  /* Pass 5: Half-res two-pass Gaussian bloom composite (High quality only) */
  if (use_bloom) {
    int hw = MAX(fb_w / 2, 2);
    int hh = MAX(fb_h / 2, 2);
    glDisable(GL_BLEND);
    glBindVertexArray(gl->vao_bg);

    /* 5a. Extract only genuinely bright cores/folders/selected (threshold 0.68 prevents white wash) */
    glBindFramebuffer(GL_FRAMEBUFFER, gl->bloom_fbo[0]);
    glViewport(0, 0, hw, hh);
    glUseProgram(gl->prog_extract);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->scene_tex);
    glUniform1i(gl->u_ext_scene, 0);
    glUniform1f(gl->u_ext_thresh, 0.68f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 5b. Horizontal Gaussian blur: bloom_tex[0] -> bloom_fbo[1] */
    glBindFramebuffer(GL_FRAMEBUFFER, gl->bloom_fbo[1]);
    glUseProgram(gl->prog_blur);
    glBindTexture(GL_TEXTURE_2D, gl->bloom_tex[0]);
    glUniform1i(gl->u_blur_tex, 0);
    glUniform2f(gl->u_blur_dir, 1.0f / (float)hw, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 5c. Vertical Gaussian blur: bloom_tex[1] -> bloom_fbo[0] */
    glBindFramebuffer(GL_FRAMEBUFFER, gl->bloom_fbo[0]);
    glBindTexture(GL_TEXTURE_2D, gl->bloom_tex[1]);
    glUniform2f(gl->u_blur_dir, 0.0f, 1.0f / (float)hh);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 5d. Composite scene + subtle bloom back into target_fbo */
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target_fbo);
    glViewport(0, 0, fb_w, fb_h);
    glUseProgram(gl->prog_composite);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->scene_tex);
    glUniform1i(gl->u_comp_scene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gl->bloom_tex[0]);
    glUniform1i(gl->u_comp_bloom, 1);
    glUniform1f(gl->u_comp_str, 0.18f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  /* Pass 6: Greedy screen-space culled Pango RGBA labels drawn AFTER bloom */
  if (st->graph != NULL && gl->node_count > 0 && gl->atlas_slots != NULL && gl->tex_atlas != 0 &&
      gl->lbl_cands != NULL && gl->obs_scratch != NULL) {
    guint cand_n = 0;
    guint obs_n = 0;
    float bt_eased = 1.0f - powf(1.0f - CLAMP(bt, 0.0f, 1.0f), 3.0f);
    gdouble eff_zoom = mode_3d ? CLAMP(2.6 / MAX(st->dist3d, 1.2), 0.5, 2.0) : st->zoom;
    guint limit_n = MIN(gl->node_count, MIN(gl->atlas_slot_count, gl->lbl_scratch_cap));

    for (guint i = 0; i < limit_n; i++) {
      const BtGraphNode *nd = g_ptr_array_index(st->graph->nodes, i);
      if (!nd->visible)
        continue;

      float sx = 0.0f, sy = 0.0f, core_px = 0.0f;
      gdouble depth_w = 1.0;
      if (mode_3d) {
        if (i >= gl->pts3d_count)
          continue;
        BtGraphVec3 pt = {gl->pts3d[i].x * bt_eased, gl->pts3d[i].y * bt_eased,
                          gl->pts3d[i].z * bt_eased};
        gdouble psx = 0.0, psy = 0.0, pw = 0.0;
        if (!bt_graph_project_3d(mvp, &pt, (gdouble)vp_w, (gdouble)vp_h, &psx, &psy, &pw))
          continue;
        sx = (float)psx;
        sy = (float)psy;
        depth_w = pw;
        float persp = CLAMP(2.4f / MAX((float)pw, 0.35f), 0.35f, 2.4f);
        core_px = CLAMP((float)nd->radius * persp * 0.85f, 2.0f, 14.0f);
      } else {
        sx = vp_w * 0.5f + (float)st->pan_x + (float)nd->x * bt_eased * (float)st->zoom;
        sy = vp_h * 0.5f + (float)st->pan_y + (float)nd->y * bt_eased * (float)st->zoom;
        core_px = (float)bt_graph_screen_core_radius(nd->radius, st->zoom);
      }

      guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
      gboolean is_sel = ((gint)i == st->selected_idx);
      gboolean is_kbd = ((gint)i == st->focus_idx);
      gboolean is_hov = ((gint)i == st->hover_idx) || (nbr == BT_NBR_FOCUS);
      gboolean is_active_node =
          has_focus ? (nbr != BT_NBR_NONE || is_sel || is_kbd)
                    : ((st->hub_thresh > 0u && nd->degree >= st->hub_thresh) ||
                       gl->node_count <= 32u || is_sel || is_kbd);

      if (is_active_node &&
          sx + core_px >= -20.0f && sx - core_px <= vp_w + 20.0f &&
          sy + core_px >= -20.0f && sy - core_px <= vp_h + 20.0f) {
        gl->obs_scratch[obs_n].cx = sx;
        gl->obs_scratch[obs_n].cy = sy;
        gl->obs_scratch[obs_n].r = core_px;
        gl->obs_scratch[obs_n].node_idx = i;
        obs_n++;
      }

      if (!gl->atlas_slots[i].valid)
        continue;
      gdouble la = bt_graph_label_alpha(nd, nbr, is_sel, is_kbd, st->hub_thresh, eff_zoom,
                                        has_focus, (gdouble)hf, gl->node_count);
      if (la < 0.06)
        continue;
      if (mode_3d && nbr == BT_NBR_NONE && !is_sel && !is_kbd) {
        la *= CLAMP(1.42 - depth_w * 0.22, 0.25, 1.0);
        if (la < 0.06)
          continue;
      }

      const AtlasSlot *sl = &gl->atlas_slots[i];
      if (sx + sl->w_log * 0.5f < 0.0f || sx - sl->w_log * 0.5f > vp_w ||
          sy + sl->h_log + 24.0f < 0.0f || sy - 24.0f > vp_h)
        continue;

      guint8 prio = 1u;
      if (is_kbd || is_sel) prio = 6u;
      else if (is_hov) prio = 5u;
      else if (nd->kind == BT_NODE_FOLDER) prio = 4u;
      else if (nbr == BT_NBR_NEIGHBOR) prio = 3u;
      else if (nd->degree >= st->hub_thresh && st->hub_thresh > 0u) prio = 2u;

      BtGraphLabelCand *c = &gl->lbl_cands[cand_n++];
      c->node_idx = i;
      c->x = floorf(sx - sl->w_log * 0.5f);
      c->y = floorf(sy + core_px + 5.0f);
      c->w = sl->w_log;
      c->h = sl->h_log;
      c->anchor_x = sx;
      c->anchor_y = sy;
      c->anchor_r = core_px;
      c->alpha = (float)la;
      c->degree = (nd->kind == BT_NODE_FOLDER) ? (nd->child_count + 100u) : nd->degree;
      c->priority = prio;
      c->always_show = (is_kbd || is_sel || is_hov);
      c->is_neighbor = (nbr == BT_NBR_NEIGHBOR && !c->always_show);
      c->pill = (c->always_show || nd->kind == BT_NODE_FOLDER);
    }

    guint max_labels = bt_graph_label_cap_for_zoom(eff_zoom);
    guint lbl_n = bt_graph_place_labels(gl->lbl_cands, cand_n, gl->obs_scratch, obs_n,
                                        max_labels, 12u, gl->lbl_placed, BT_GL_LABEL_CAP);
    gl->last_label_count = lbl_n;
    guint ov_cnt = 0;
    for (guint i = 0; i < lbl_n; i++) {
      for (guint j = i + 1; j < lbl_n; j++) {
        const BtGraphLabelCand *la_box = &gl->lbl_placed[i];
        const BtGraphLabelCand *lb_box = &gl->lbl_placed[j];
        if (la_box->x < lb_box->x + lb_box->w && la_box->x + la_box->w > lb_box->x &&
            la_box->y < lb_box->y + lb_box->h && la_box->y + la_box->h > lb_box->y) {
          ov_cnt++;
        }
      }
    }
    gl->last_overlap_count = ov_cnt;

    for (guint k = 0; k < lbl_n; k++) {
      /* Draw lower priority first so primary focused/selected pill renders on top. */
      const BtGraphLabelCand *p = &gl->lbl_placed[lbl_n - 1u - k];
      const AtlasSlot *sl = &gl->atlas_slots[p->node_idx];
      LabelInst *li = &gl->label_scratch[k];
      li->x = p->x;
      li->y = p->y;
      li->w = p->w;
      li->h = p->h;
      li->u0 = sl->u0;
      li->v0 = sl->v0;
      li->u1 = sl->u1;
      li->v1 = sl->v1;
      li->alpha = p->alpha;
      li->pill = p->pill ? 1.0f : 0.0f;
    }

    if (lbl_n > 0) {
      glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_label);
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(lbl_n * sizeof(LabelInst)),
                      gl->label_scratch);
      glBindBuffer(GL_ARRAY_BUFFER, 0);

      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glUseProgram(gl->prog_label);
      glUniform2f(gl->u_lbl_vp, vp_w, vp_h);
      glUniform1i(gl->u_lbl_atlas, 0);
      bt_theme_unpack_f(BT_COL_TEXT_PRIMARY, &r, &g, &b);
      glUniform3f(gl->u_lbl_tp, r, g, b);
      bt_theme_unpack_f(BT_COL_TEXT_SECONDARY, &r, &g, &b);
      glUniform3f(gl->u_lbl_ts, r, g, b);
      bt_theme_unpack_f(BT_COL_SURFACE_2, &r, &g, &b);
      glUniform3f(gl->u_lbl_pbg, r, g, b);
      bt_theme_unpack_f(BT_COL_GRAPH_BG, &r, &g, &b);
      glUniform3f(gl->u_lbl_bg, r, g, b);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, gl->tex_atlas);
      glBindVertexArray(gl->vao_label);
      glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)lbl_n);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }

  glBindVertexArray(0);
  glUseProgram(0);
}

/* ------------------------------------------- GtkGLArea signal handlers -- */

static void on_realize(GtkWidget *widget, gpointer ud)
{
  BtGraphGL *gl = ud;
  GtkGLArea *area = GTK_GL_AREA(widget);
  gint64 t0 = g_get_monotonic_time();

  gtk_gl_area_make_current(area);
  GError *gl_err = gtk_gl_area_get_error(area);

  BtGraphGlProbe probe;
  memset(&probe, 0, sizeof(probe));
  const gchar *env_be = g_getenv("BRAINTERM_GRAPH_BACKEND");
  probe.force_cairo_env = (env_be != NULL && g_ascii_strcasecmp(env_be, "cairo") == 0);
  probe.gl_context_ok = (gl_err == NULL && gtk_gl_area_get_context(area) != NULL);

  if (probe.gl_context_ok) {
    GdkGLContext *ctx = gtk_gl_area_get_context(area);
    probe.is_gles = gdk_gl_context_get_use_es(ctx);
    int ver = epoxy_gl_version();
    probe.gl_major = ver / 10;
    probe.gl_minor = ver % 10;

    const char *ven = (const char *)glGetString(GL_VENDOR);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    const char *ver_s = (const char *)glGetString(GL_VERSION);
    g_strlcpy(gl->vendor_str, ven ? ven : "unknown", sizeof(gl->vendor_str));
    g_strlcpy(gl->renderer_str, ren ? ren : "unknown", sizeof(gl->renderer_str));
    g_strlcpy(gl->version_str, ver_s ? ver_s : "unknown", sizeof(gl->version_str));

    gboolean version_ok = probe.is_gles
        ? (probe.gl_major >= 3)
        : (probe.gl_major > 3 || (probe.gl_major == 3 && probe.gl_minor >= 3));
    if (version_ok && !probe.force_cairo_env) {
      gl->prog_bg = link_program("graph_bg.vert.glsl", "graph_bg.frag.glsl", probe.is_gles);
      gl->prog_dust = link_program("graph_dust.vert.glsl", "graph_dust.frag.glsl", probe.is_gles);
      gl->prog_edge = link_program("graph_edge.vert.glsl", "graph_edge.frag.glsl", probe.is_gles);
      gl->prog_node = link_program("graph_node.vert.glsl", "graph_node.frag.glsl", probe.is_gles);
      gl->prog_label = link_program("graph_label.vert.glsl", "graph_label.frag.glsl", probe.is_gles);
      probe.shaders_ok = (gl->prog_bg && gl->prog_dust && gl->prog_edge &&
                          gl->prog_node && gl->prog_label);

      /* Bloom shaders are optional: if they fail, downgrade bloom off without failing GL. */
      gl->prog_extract =
          link_program("graph_bg.vert.glsl", "graph_bloom_extract.frag.glsl", probe.is_gles);
      gl->prog_blur =
          link_program("graph_bg.vert.glsl", "graph_bloom_blur.frag.glsl", probe.is_gles);
      gl->prog_composite =
          link_program("graph_bg.vert.glsl", "graph_bloom_composite.frag.glsl", probe.is_gles);
      probe.fbo_ok = (gl->prog_extract && gl->prog_blur && gl->prog_composite);
    }
  }

  BtGraphBackendDecision dec;
  bt_graph_select_backend(&probe, &dec);
  if (dec.backend != BT_GRAPH_BACKEND_GL) {
    destroy_gl_objects(gl);
    gl->gl_failed = TRUE;
    gl->init_ms = (gdouble)(g_get_monotonic_time() - t0) / 1000.0;
    if (gl->on_fallback != NULL)
      gl->on_fallback(dec.reason, gl->user_data);
    return;
  }

  gl->bloom_supported = dec.bloom_enabled;

  /* Cache uniform locations once. */
  gl->u_bg_col = glGetUniformLocation(gl->prog_bg, "u_bg_col");
  gl->u_bg_vig = glGetUniformLocation(gl->prog_bg, "u_vig_col");

  gl->u_dust_vp = glGetUniformLocation(gl->prog_dust, "u_viewport");
  gl->u_dust_time = glGetUniformLocation(gl->prog_dust, "u_time");
  gl->u_dust_scale = glGetUniformLocation(gl->prog_dust, "u_scale");
  gl->u_dust_col = glGetUniformLocation(gl->prog_dust, "u_dust_col");
  gl->u_dust_int = glGetUniformLocation(gl->prog_dust, "u_intensity");

  gl->u_edge_vp = glGetUniformLocation(gl->prog_edge, "u_viewport");
  gl->u_edge_pan = glGetUniformLocation(gl->prog_edge, "u_pan");
  gl->u_edge_zoom = glGetUniformLocation(gl->prog_edge, "u_zoom");
  gl->u_edge_scale = glGetUniformLocation(gl->prog_edge, "u_scale");
  gl->u_edge_bt = glGetUniformLocation(gl->prog_edge, "u_birth_t");
  gl->u_edge_foc = glGetUniformLocation(gl->prog_edge, "u_focus_idx");
  gl->u_edge_sel = glGetUniformLocation(gl->prog_edge, "u_selected_idx");
  gl->u_edge_hf = glGetUniformLocation(gl->prog_edge, "u_hover_fade");
  gl->u_edge_segs = glGetUniformLocation(gl->prog_edge, "u_seg_count");
  gl->u_edge_curved = glGetUniformLocation(gl->prog_edge, "u_curved");
  gl->u_edge_ba = glGetUniformLocation(gl->prog_edge, "u_base_alpha");
  gl->u_edge_hw = glGetUniformLocation(gl->prog_edge, "u_half_width");
  gl->u_edge_dim_col = glGetUniformLocation(gl->prog_edge, "u_dim_col");
  gl->u_edge_act_col = glGetUniformLocation(gl->prog_edge, "u_active_col");
  gl->u_edge_mode3d = glGetUniformLocation(gl->prog_edge, "u_mode_3d");
  gl->u_edge_mvp = glGetUniformLocation(gl->prog_edge, "u_mvp");
  gl->u_edge_pulse_en = glGetUniformLocation(gl->prog_edge, "u_pulse_enabled");
  gl->u_edge_pulse_ph = glGetUniformLocation(gl->prog_edge, "u_pulse_phase");
  gl->u_edge_show_links = glGetUniformLocation(gl->prog_edge, "u_show_links");

  gl->u_node_vp = glGetUniformLocation(gl->prog_node, "u_viewport");
  gl->u_node_pan = glGetUniformLocation(gl->prog_node, "u_pan");
  gl->u_node_zoom = glGetUniformLocation(gl->prog_node, "u_zoom");
  gl->u_node_scale = glGetUniformLocation(gl->prog_node, "u_scale");
  gl->u_node_bt = glGetUniformLocation(gl->prog_node, "u_birth_t");
  gl->u_node_has_foc = glGetUniformLocation(gl->prog_node, "u_has_focus");
  gl->u_node_hf = glGetUniformLocation(gl->prog_node, "u_hover_fade");
  gl->u_node_hq = glGetUniformLocation(gl->prog_node, "u_high_quality");
  gl->u_node_glow_col = glGetUniformLocation(gl->prog_node, "u_glow_col");
  gl->u_node_s2_col = glGetUniformLocation(gl->prog_node, "u_surface2_col");
  gl->u_node_mode3d = glGetUniformLocation(gl->prog_node, "u_mode_3d");
  gl->u_node_mvp = glGetUniformLocation(gl->prog_node, "u_mvp");
  gl->u_node_mouse = glGetUniformLocation(gl->prog_node, "u_mouse_px");
  gl->u_node_spread = glGetUniformLocation(gl->prog_node, "u_spread_str");
  gl->u_node_dim_alpha = glGetUniformLocation(gl->prog_node, "u_dim_alpha");

  gl->u_lbl_vp = glGetUniformLocation(gl->prog_label, "u_viewport");
  gl->u_lbl_atlas = glGetUniformLocation(gl->prog_label, "u_atlas");
  gl->u_lbl_tp = glGetUniformLocation(gl->prog_label, "u_text_primary");
  gl->u_lbl_ts = glGetUniformLocation(gl->prog_label, "u_text_secondary");
  gl->u_lbl_pbg = glGetUniformLocation(gl->prog_label, "u_pill_bg");
  gl->u_lbl_bg = glGetUniformLocation(gl->prog_label, "u_bg_col");

  if (gl->bloom_supported) {
    gl->u_ext_scene = glGetUniformLocation(gl->prog_extract, "u_scene");
    gl->u_ext_thresh = glGetUniformLocation(gl->prog_extract, "u_threshold");
    gl->u_blur_tex = glGetUniformLocation(gl->prog_blur, "u_tex");
    gl->u_blur_dir = glGetUniformLocation(gl->prog_blur, "u_dir");
    gl->u_comp_scene = glGetUniformLocation(gl->prog_composite, "u_scene");
    gl->u_comp_bloom = glGetUniformLocation(gl->prog_composite, "u_bloom");
    gl->u_comp_str = glGetUniformLocation(gl->prog_composite, "u_strength");
  }

  init_dust_vbo(gl);
  init_dynamic_vaos(gl);
  gl->gl_ready = TRUE;
  gl->graph_dirty = TRUE;
  gl->layout3d_dirty = TRUE;
  gl->state_dirty = TRUE;
  gl->atlas_dirty = TRUE;
  gl->init_ms = (gdouble)(g_get_monotonic_time() - t0) / 1000.0;
}

static void on_unrealize(GtkWidget *widget, gpointer ud)
{
  BtGraphGL *gl = ud;
  gtk_gl_area_make_current(GTK_GL_AREA(widget));
  if (gtk_gl_area_get_error(GTK_GL_AREA(widget)) == NULL)
    destroy_gl_objects(gl);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *context, gpointer ud)
{
  (void)context;
  BtGraphGL *gl = ud;
  if (!gl->gl_ready || gl->fill_state == NULL)
    return TRUE;
  BtGraphRenderState st;
  memset(&st, 0, sizeof(st));
  st.width = gtk_widget_get_width(GTK_WIDGET(area));
  st.height = gtk_widget_get_height(GTK_WIDGET(area));
  st.scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
  gl->fill_state(&st, gl->user_data);
  bt_graph_gl_render_state(gl, &st);
  return TRUE;
}

/* ---------------------------------------------------------- public API -- */

BtGraphGL *bt_graph_gl_new(BtGraphGLStateFillFn fill_state, BtGraphGLFallbackFn on_fallback,
                           gpointer user_data)
{
  BtGraphGL *gl = g_new0(BtGraphGL, 1);
  gl->fill_state = fill_state;
  gl->on_fallback = on_fallback;
  gl->user_data = user_data;
  gl->graph_dirty = TRUE;
  gl->layout3d_dirty = TRUE;
  gl->state_dirty = TRUE;
  gl->atlas_dirty = TRUE;

  gl->area = gtk_gl_area_new();
  gtk_widget_set_name(gl->area, "bt-graph-gl");
  gtk_widget_set_hexpand(gl->area, TRUE);
  gtk_widget_set_vexpand(gl->area, TRUE);
  gtk_widget_set_size_request(gl->area, 200, 160);
  gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(gl->area), FALSE);
  gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(gl->area), FALSE);
  gtk_gl_area_set_auto_render(GTK_GL_AREA(gl->area), TRUE);

  g_signal_connect(gl->area, "realize", G_CALLBACK(on_realize), gl);
  g_signal_connect(gl->area, "unrealize", G_CALLBACK(on_unrealize), gl);
  g_signal_connect(gl->area, "render", G_CALLBACK(on_render), gl);
  return gl;
}

void bt_graph_gl_free(BtGraphGL *gl)
{
  if (gl == NULL)
    return;
  g_free(gl->pts3d);
  g_free(gl->node_state_scratch);
  g_free(gl->atlas_slots);
  g_free(gl->lbl_cands);
  g_free(gl->obs_scratch);
  g_free(gl);
}

GtkWidget *bt_graph_gl_widget(BtGraphGL *gl)
{
  return gl ? gl->area : NULL;
}

void bt_graph_gl_set_graph(BtGraphGL *gl, const BtGraph *graph)
{
  if (gl == NULL)
    return;
  gl->graph = graph;
  gl->graph_dirty = TRUE;
  gl->layout3d_dirty = TRUE;
  gl->state_dirty = TRUE;
  gl->atlas_dirty = TRUE;
}

void bt_graph_gl_set_layout3d(BtGraphGL *gl, const BtGraphVec3 *pts, guint count)
{
  if (gl == NULL)
    return;
  g_free(gl->pts3d);
  gl->pts3d = NULL;
  gl->pts3d_count = 0;
  if (pts != NULL && count > 0) {
    gl->pts3d = g_memdup2(pts, count * sizeof(BtGraphVec3));
    gl->pts3d_count = count;
  }
  gl->layout3d_dirty = TRUE;
}

void bt_graph_gl_mark_graph_dirty(BtGraphGL *gl)
{
  if (gl != NULL) {
    gl->graph_dirty = TRUE;
    gl->atlas_dirty = TRUE;
    gl->state_dirty = TRUE;
  }
}

void bt_graph_gl_mark_state_dirty(BtGraphGL *gl)
{
  if (gl != NULL)
    gl->state_dirty = TRUE;
}

void bt_graph_gl_queue_render(BtGraphGL *gl)
{
  if (gl != NULL && gl->area != NULL && gl->gl_ready)
    gtk_gl_area_queue_render(GTK_GL_AREA(gl->area));
}

gboolean bt_graph_gl_is_ready(const BtGraphGL *gl)
{
  return gl != NULL && gl->gl_ready;
}

gboolean bt_graph_gl_has_failed(const BtGraphGL *gl)
{
  return gl != NULL && gl->gl_failed;
}

gboolean bt_graph_gl_bloom_ready(const BtGraphGL *gl)
{
  return gl != NULL && gl->bloom_supported;
}

const gchar *bt_graph_gl_vendor(const BtGraphGL *gl)
{
  return gl ? gl->vendor_str : "";
}

const gchar *bt_graph_gl_renderer(const BtGraphGL *gl)
{
  return gl ? gl->renderer_str : "";
}

const gchar *bt_graph_gl_version(const BtGraphGL *gl)
{
  return gl ? gl->version_str : "";
}

gdouble bt_graph_gl_init_ms(const BtGraphGL *gl)
{
  return gl ? gl->init_ms : 0.0;
}

guint bt_graph_gl_last_label_count(const BtGraphGL *gl)
{
  return gl ? gl->last_label_count : 0u;
}

guint bt_graph_gl_last_overlap_count(const BtGraphGL *gl)
{
  return gl ? gl->last_overlap_count : 0u;
}

guint bt_graph_gl_visible_edge_count(const BtGraphGL *gl)
{
  return gl ? gl->edge_count : 0u;
}

gboolean bt_graph_gl_capture_png(BtGraphGL *gl, const BtGraphRenderState *st, const gchar *png_path,
                                 GError **error)
{
  if (gl == NULL || !gl->gl_ready || gl->area == NULL || st == NULL || png_path == NULL ||
      st->width <= 0 || st->height <= 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "GL graph capture unavailable");
    return FALSE;
  }

  gtk_gl_area_make_current(GTK_GL_AREA(gl->area));
  if (gtk_gl_area_get_error(GTK_GL_AREA(gl->area)) != NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "GL context error during capture");
    return FALSE;
  }

  gint scale = MAX(st->scale, 1);
  int fb_w = st->width * scale;
  int fb_h = st->height * scale;

  GLint prev_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);

  GLuint fbo = 0, tex = 0;
  if (!alloc_color_fbo(&fbo, &tex, fb_w, fb_h)) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate GL capture FBO");
    return FALSE;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  bt_graph_gl_render_state(gl, st);
  glFinish();

  g_autofree unsigned char *rgba = g_malloc((gsize)fb_w * (gsize)fb_h * 4u);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
  glDeleteTextures(1, &tex);
  glDeleteFramebuffers(1, &fbo);

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fb_w, fb_h);
  unsigned char *dst = cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf);
  for (int y = 0; y < fb_h; y++) {
    const unsigned char *srow = rgba + (gsize)(fb_h - 1 - y) * (gsize)fb_w * 4u;
    unsigned char *drow = dst + y * stride;
    for (int x = 0; x < fb_w; x++) {
      drow[x * 4 + 0] = srow[x * 4 + 2];
      drow[x * 4 + 1] = srow[x * 4 + 1];
      drow[x * 4 + 2] = srow[x * 4 + 0];
      drow[x * 4 + 3] = 255;
    }
  }
  cairo_surface_mark_dirty(surf);
  cairo_status_t cst = cairo_surface_write_to_png(surf, png_path);
  cairo_surface_destroy(surf);
  if (cst != CAIRO_STATUS_SUCCESS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write PNG '%s': %s",
                png_path, cairo_status_to_string(cst));
    return FALSE;
  }
  return TRUE;
}
