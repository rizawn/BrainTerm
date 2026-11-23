/* graphgl.h — OpenGL 3.3 Core / GLES 3.0 Brain Graph v2 renderer on GtkGLArea.
 * Instanced luminous nodes, additive curved/straight edges, GPU dust sprites,
 * and a cached Pango RGBA label atlas. Falls back cleanly to Cairo on any GL error. */
#ifndef BT_GRAPHGL_H
#define BT_GRAPHGL_H

#include <gtk/gtk.h>

#include "graph.h"
#include "graph_state.h"
#include "ui/graphview_renderer.h"

G_BEGIN_DECLS

typedef struct BtGraphGL BtGraphGL;

typedef void (*BtGraphGLStateFillFn)(BtGraphRenderState *out, gpointer user_data);
typedef void (*BtGraphGLFallbackFn)(const gchar *reason, gpointer user_data);

BtGraphGL *bt_graph_gl_new(BtGraphGLStateFillFn fill_state, BtGraphGLFallbackFn on_fallback,
                           gpointer user_data);
void bt_graph_gl_free(BtGraphGL *gl);

GtkWidget *bt_graph_gl_widget(BtGraphGL *gl);
void bt_graph_gl_set_graph(BtGraphGL *gl, const BtGraph *graph);
void bt_graph_gl_set_layout3d(BtGraphGL *gl, const BtGraphVec3 *pts, guint count);
void bt_graph_gl_mark_graph_dirty(BtGraphGL *gl);
void bt_graph_gl_mark_state_dirty(BtGraphGL *gl);
void bt_graph_gl_queue_render(BtGraphGL *gl);

gboolean bt_graph_gl_is_ready(const BtGraphGL *gl);
gboolean bt_graph_gl_has_failed(const BtGraphGL *gl);
gboolean bt_graph_gl_bloom_ready(const BtGraphGL *gl);
const gchar *bt_graph_gl_vendor(const BtGraphGL *gl);
const gchar *bt_graph_gl_renderer(const BtGraphGL *gl);
const gchar *bt_graph_gl_version(const BtGraphGL *gl);
gdouble bt_graph_gl_init_ms(const BtGraphGL *gl);
guint bt_graph_gl_last_label_count(const BtGraphGL *gl);
guint bt_graph_gl_last_overlap_count(const BtGraphGL *gl);
guint bt_graph_gl_visible_edge_count(const BtGraphGL *gl);

/* Direct draw helper for headless/offscreen GL benchmarks (caller must bind FBO & make_current). */
void bt_graph_gl_render_state(BtGraphGL *gl, const BtGraphRenderState *st);

/* Captures current state rendered via GL offscreen FBO into a PNG file. */
gboolean bt_graph_gl_capture_png(BtGraphGL *gl, const BtGraphRenderState *st, const gchar *png_path,
                                 GError **error);

G_END_DECLS

#endif
