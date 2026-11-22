/* graphgl.c — OpenGL 3.3 Core / GLES 3.0 Brain Graph v2 backend (Step 2):
 *   - Instanced quads for glowing nodes (analytic halo + bright core + hover ring).
 *   - Instanced curved/straight additive ribbon strips for edges.
 *   - GPU-animated drifting dust particles (zero per-frame CPU upload).
 *   - Cached Pango RGBA label atlas rebuilt only on graph or scale change.
 *   - Zero per-frame heap allocations; automatic Cairo fallback on any GL/shader error. */
#include "ui/graphgl.h"
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
  gboolean gl_ready;
  gboolean gl_failed;
  gboolean graph_dirty;
  gboolean state_dirty;
  gboolean atlas_dirty;
  gint atlas_scale;

  gchar vendor_str[128];
  gchar renderer_str[128];
  gchar version_str[128];
  gdouble init_ms;

  /* Shader programs */
  GLuint prog_bg;
  GLuint prog_dust;
  GLuint prog_edge;
  GLuint prog_node;
  GLuint prog_label;

  /* Cached uniform locations */
  GLint u_bg_col, u_bg_vig;
  GLint u_dust_vp, u_dust_time, u_dust_scale, u_dust_col, u_dust_int;
  GLint u_edge_vp, u_edge_pan, u_edge_zoom, u_edge_scale, u_edge_bt;
  GLint u_edge_foc, u_edge_hf, u_edge_segs, u_edge_curved;
  GLint u_edge_ba, u_edge_hw, u_edge_dim_col, u_edge_act_col;
  GLint u_node_vp, u_node_pan, u_node_zoom, u_node_scale, u_node_bt;
  GLint u_node_has_foc, u_node_hf, u_node_hq, u_node_glow_col, u_node_s2_col;
  GLint u_lbl_vp, u_lbl_atlas, u_lbl_tp, u_lbl_ts, u_lbl_pbg;

  /* VAOs & VBOs */
  GLuint vao_bg;
  GLuint vao_dust, vbo_dust;
  GLuint vao_edge, vbo_edge;
  GLuint vao_node, vbo_node_static, vbo_node_state;
  GLuint vao_label, vbo_label;
  GLuint tex_atlas;

  guint edge_count;
  guint node_count;
  guint node_state_cap;
  float *node_state_scratch; /* reusable per-node state buffer (zero per-frame alloc) */

  AtlasSlot *atlas_slots;
  guint atlas_slot_count;
  LabelInst label_scratch[BT_GL_LABEL_CAP];
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
  case BT_NODE_HUMAN_NOTE:  return BT_COL_NODE_NOTE;
  case BT_NODE_AI_NOTE:     return BT_COL_NODE_AI;
  case BT_NODE_TAG:         return BT_COL_NODE_TAG;
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

/* ----------------------------------------------------- GL resource mgmt -- */

static void destroy_gl_objects(BtGraphGL *gl)
{
  if (gl->prog_bg)    { glDeleteProgram(gl->prog_bg);    gl->prog_bg = 0; }
  if (gl->prog_dust)  { glDeleteProgram(gl->prog_dust);  gl->prog_dust = 0; }
  if (gl->prog_edge)  { glDeleteProgram(gl->prog_edge);  gl->prog_edge = 0; }
  if (gl->prog_node)  { glDeleteProgram(gl->prog_node);  gl->prog_node = 0; }
  if (gl->prog_label) { glDeleteProgram(gl->prog_label); gl->prog_label = 0; }

  if (gl->vao_bg)    { glDeleteVertexArrays(1, &gl->vao_bg);    gl->vao_bg = 0; }
  if (gl->vao_dust)  { glDeleteVertexArrays(1, &gl->vao_dust);  gl->vao_dust = 0; }
  if (gl->vao_edge)  { glDeleteVertexArrays(1, &gl->vao_edge);  gl->vao_edge = 0; }
  if (gl->vao_node)  { glDeleteVertexArrays(1, &gl->vao_node);  gl->vao_node = 0; }
  if (gl->vao_label) { glDeleteVertexArrays(1, &gl->vao_label); gl->vao_label = 0; }

  if (gl->vbo_dust)        { glDeleteBuffers(1, &gl->vbo_dust);        gl->vbo_dust = 0; }
  if (gl->vbo_edge)        { glDeleteBuffers(1, &gl->vbo_edge);        gl->vbo_edge = 0; }
  if (gl->vbo_node_static) { glDeleteBuffers(1, &gl->vbo_node_static); gl->vbo_node_static = 0; }
  if (gl->vbo_node_state)  { glDeleteBuffers(1, &gl->vbo_node_state);  gl->vbo_node_state = 0; }
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

  /* Edges VAO */
  glGenVertexArrays(1, &gl->vao_edge);
  glGenBuffers(1, &gl->vbo_edge);
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

  /* Nodes VAO (static geom/color + dynamic state) */
  glGenVertexArrays(1, &gl->vao_node);
  glGenBuffers(1, &gl->vbo_node_static);
  glGenBuffers(1, &gl->vbo_node_state);
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

  /* Labels VAO + pre-allocated VBO (BT_GL_LABEL_CAP instances) */
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

/* ------------------------------------------- graph & label atlas upload -- */

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
    pango_layout_set_text(pl, nd->label, -1);
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
      break; /* atlas full (covers hundreds of labels) */

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

  /* Convert Cairo ARGB32 (BGRA on little-endian) -> GL_RGBA once on atlas build. */
  int used_h = MIN(MAX(cy + row_h + 4, 64), ah);
  unsigned char *src = cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf);
  g_autofree unsigned char *rgba = g_malloc((gsize)aw * (gsize)used_h * 4u);
  for (int y = 0; y < used_h; y++) {
    const unsigned char *srow = src + y * stride;
    unsigned char *drow = rgba + (gsize)y * (gsize)aw * 4u;
    for (int x = 0; x < aw; x++) {
      drow[x * 4 + 0] = srow[x * 4 + 2]; /* R */
      drow[x * 4 + 1] = srow[x * 4 + 1]; /* G */
      drow[x * 4 + 2] = srow[x * 4 + 0]; /* B */
      drow[x * 4 + 3] = srow[x * 4 + 3]; /* A */
    }
  }
  /* Rescale V coordinates to tightly cropped used_h. */
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
    bt_theme_unpack_f(kind_rgb(nd->kind), &cr, &cg, &cb);
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

  gint scale = MAX(st->scale, 1);
  if (gl->atlas_dirty || gl->atlas_scale != scale)
    rebuild_label_atlas(gl, scale);
  if (gl->state_dirty)
    upload_node_states(gl, st);

  int fb_w = st->width * scale;
  int fb_h = st->height * scale;
  glViewport(0, 0, fb_w, fb_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  float r, g, b;

  /* Pass 1: dark canvas + subtle radial vignette */
  glDisable(GL_BLEND);
  glUseProgram(gl->prog_bg);
  bt_theme_unpack_f(BT_COL_GRAPH_BG, &r, &g, &b);
  glUniform3f(gl->u_bg_col, r, g, b);
  bt_theme_unpack_f(BT_COL_GRAPH_VIGNETTE, &r, &g, &b);
  glUniform3f(gl->u_bg_vig, r, g, b);
  glBindVertexArray(gl->vao_bg);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  gboolean high = (st->quality == BT_GRAPH_QUALITY_HIGH);
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

  if (st->graph == NULL || gl->node_count == 0) {
    glBindVertexArray(0);
    glUseProgram(0);
    return;
  }

  /* Compute shared birth_t (min across nodes if animating; O(1) fast check on node 0). */
  float bt = 1.0f;
  if (st->graph->nodes->len > 0) {
    const BtGraphNode *n0 = g_ptr_array_index(st->graph->nodes, 0);
    if (n0->birth_t < 0.999)
      bt = (float)n0->birth_t;
  }

  gint focus_idx = (st->hover_idx >= 0) ? st->hover_idx : st->focus_idx;
  gboolean has_focus = (focus_idx >= 0 && (guint)focus_idx < gl->node_count);
  float hf = has_focus ? (float)CLAMP(st->hover_fade, 0.0, 1.0) : 0.0f;
  if (st->hover_idx < 0 && st->focus_idx >= 0)
    hf = 1.0f;

  /* Pass 3: instanced edges (additive blending so crossings glow brighter) */
  if (gl->edge_count > 0) {
    int segs = high ? 6 : 1;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glUseProgram(gl->prog_edge);
    glUniform2f(gl->u_edge_vp, vp_w, vp_h);
    glUniform2f(gl->u_edge_pan, (float)st->pan_x, (float)st->pan_y);
    glUniform1f(gl->u_edge_zoom, (float)st->zoom);
    glUniform1f(gl->u_edge_scale, 1.0f);
    glUniform1f(gl->u_edge_bt, bt);
    glUniform1i(gl->u_edge_foc, focus_idx);
    glUniform1f(gl->u_edge_hf, hf);
    glUniform1i(gl->u_edge_segs, segs);
    glUniform1i(gl->u_edge_curved, high ? 1 : 0);
    glUniform1f(gl->u_edge_ba, high ? 0.20f : 0.14f);
    glUniform1f(gl->u_edge_hw, high ? 0.85f : 0.70f);
    bt_theme_unpack_f(BT_COL_GRAPH_EDGE_DIM, &r, &g, &b);
    glUniform3f(gl->u_edge_dim_col, r, g, b);
    bt_theme_unpack_f(BT_COL_GRAPH_EDGE_ACTIVE, &r, &g, &b);
    glUniform3f(gl->u_edge_act_col, r, g, b);
    glBindVertexArray(gl->vao_edge);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, (segs + 1) * 2, (GLsizei)gl->edge_count);
  }

  /* Pass 4: instanced glowing nodes */
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
  glBindVertexArray(gl->vao_node);
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)gl->node_count);

  /* Pass 5: cached Pango RGBA label atlas (zero per-frame heap alloc) */
  if (gl->atlas_slots != NULL && gl->tex_atlas != 0) {
    guint lbl_n = 0;
    float bt_eased = 1.0f - powf(1.0f - CLAMP(bt, 0.0f, 1.0f), 3.0f);
    for (guint i = 0; i < gl->node_count && i < gl->atlas_slot_count && lbl_n < BT_GL_LABEL_CAP; i++) {
      if (!gl->atlas_slots[i].valid)
        continue;
      const BtGraphNode *nd = g_ptr_array_index(st->graph->nodes, i);
      guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
      gboolean is_sel = ((gint)i == st->selected_idx);
      gboolean is_kbd = ((gint)i == st->focus_idx);
      gdouble la = bt_graph_label_alpha(nd, nbr, is_sel, is_kbd, st->hub_thresh, st->zoom,
                                        has_focus, (gdouble)hf, gl->node_count);
      if (la < 0.06)
        continue;

      float sx = vp_w * 0.5f + (float)st->pan_x + (float)nd->x * bt_eased * (float)st->zoom;
      float sy = vp_h * 0.5f + (float)st->pan_y + (float)nd->y * bt_eased * (float)st->zoom;
      const AtlasSlot *sl = &gl->atlas_slots[i];
      if (sx + sl->w_log * 0.5f < 0.0f || sx - sl->w_log * 0.5f > vp_w ||
          sy + sl->h_log + 24.0f < 0.0f || sy - 24.0f > vp_h)
        continue;

      LabelInst *li = &gl->label_scratch[lbl_n++];
      li->x = floorf(sx - sl->w_log * 0.5f);
      li->y = floorf(sy + (float)nd->radius * (float)st->zoom + 5.0f);
      li->w = sl->w_log;
      li->h = sl->h_log;
      li->u0 = sl->u0;
      li->v0 = sl->v0;
      li->u1 = sl->u1;
      li->v1 = sl->v1;
      li->alpha = (float)la;
      li->pill = (nbr == BT_NBR_FOCUS || is_sel || is_kbd) ? 1.0f : 0.0f;
    }

    if (lbl_n > 0) {
      glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_label);
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(lbl_n * sizeof(LabelInst)),
                      gl->label_scratch);
      glBindBuffer(GL_ARRAY_BUFFER, 0);

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
      probe.fbo_ok = TRUE; /* FBO bloom added in Step 3 */
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
  gl->u_edge_hf = glGetUniformLocation(gl->prog_edge, "u_hover_fade");
  gl->u_edge_segs = glGetUniformLocation(gl->prog_edge, "u_seg_count");
  gl->u_edge_curved = glGetUniformLocation(gl->prog_edge, "u_curved");
  gl->u_edge_ba = glGetUniformLocation(gl->prog_edge, "u_base_alpha");
  gl->u_edge_hw = glGetUniformLocation(gl->prog_edge, "u_half_width");
  gl->u_edge_dim_col = glGetUniformLocation(gl->prog_edge, "u_dim_col");
  gl->u_edge_act_col = glGetUniformLocation(gl->prog_edge, "u_active_col");

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

  gl->u_lbl_vp = glGetUniformLocation(gl->prog_label, "u_viewport");
  gl->u_lbl_atlas = glGetUniformLocation(gl->prog_label, "u_atlas");
  gl->u_lbl_tp = glGetUniformLocation(gl->prog_label, "u_text_primary");
  gl->u_lbl_ts = glGetUniformLocation(gl->prog_label, "u_text_secondary");
  gl->u_lbl_pbg = glGetUniformLocation(gl->prog_label, "u_pill_bg");

  init_dust_vbo(gl);
  init_dynamic_vaos(gl);
  gl->gl_ready = TRUE;
  gl->graph_dirty = TRUE;
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
  g_free(gl->node_state_scratch);
  g_free(gl->atlas_slots);
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
  gl->state_dirty = TRUE;
  gl->atlas_dirty = TRUE;
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
