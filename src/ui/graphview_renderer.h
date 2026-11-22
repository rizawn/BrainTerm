/* graphview_renderer.h — Unified Brain Graph v2 renderer interface shared by
 * Cairo 2D fallback (graphview.c) and OpenGL 3.3 / GLES 3.0 backend (Step 2). */
#ifndef BT_GRAPHVIEW_RENDERER_H
#define BT_GRAPHVIEW_RENDERER_H

#include <cairo.h>
#include <glib.h>

#include "graph.h"
#include "graph_state.h"

G_BEGIN_DECLS

typedef struct {
  const BtGraph *graph;
  int width;
  int height;
  int scale;

  /* 2D camera */
  gdouble pan_x;
  gdouble pan_y;
  gdouble zoom;

  /* 3D orbit camera (used by GL backend in Step 3) */
  gdouble yaw;
  gdouble pitch;
  gdouble dist3d;

  /* Interaction & pre-computed neighborhood (caller-owned, zero per-frame alloc) */
  gint hover_idx;
  gint selected_idx;
  gint focus_idx;
  gdouble hover_fade;
  const guint8 *nbr_mask;
  guint nbr_mask_len;
  guint hub_thresh;

  /* Ambient & animation state */
  gdouble time_sec;
  gdouble pulse_phase;
  BtGraphQuality quality;
  BtGraphMode mode;
  gboolean reduce_motion;
  gboolean pulse_enabled;
} BtGraphRenderState;

typedef struct BtGraphRenderer BtGraphRenderer;

typedef struct {
  const gchar *name;
  BtGraphBackend backend;
  gboolean (*init)(BtGraphRenderer *r);
  void (*upload_graph)(BtGraphRenderer *r, const BtGraph *g);
  void (*upload_layout3d)(BtGraphRenderer *r, const BtGraphVec3 *pts, guint count);
  void (*resize)(BtGraphRenderer *r, int width, int height, int scale);
  void (*render_cairo)(BtGraphRenderer *r, cairo_t *cr, const BtGraphRenderState *st);
  void (*render_gl)(BtGraphRenderer *r, const BtGraphRenderState *st);
  void (*destroy)(BtGraphRenderer *r);
} BtGraphRendererVTable;

struct BtGraphRenderer {
  const BtGraphRendererVTable *vt;
  gpointer priv;
};

/* Creates the Cairo 2D renderer backend (always available, zero GL dependency). */
BtGraphRenderer *bt_graph_cairo_renderer_new(void);

G_END_DECLS

#endif
