/* graph_state.h — Pure-C (GTK-free) Brain Graph v2 logic:
 *   - Renderer backend selection & GL fallback decision rules.
 *   - Zero-allocation hover/selection 1-hop neighbor bitmask computation.
 *   - Deterministic label visibility & fade alpha rules.
 *   - Deterministic 3D cortex/spine/brainstem layout projection from BtGraph. */
#ifndef BT_GRAPH_STATE_H
#define BT_GRAPH_STATE_H

#include <glib.h>

#include "graph.h"

G_BEGIN_DECLS

typedef enum {
  BT_GRAPH_BACKEND_CAIRO = 0,
  BT_GRAPH_BACKEND_GL = 1
} BtGraphBackend;

typedef enum {
  BT_GRAPH_QUALITY_LOW = 0,
  BT_GRAPH_QUALITY_HIGH = 1
} BtGraphQuality;

typedef enum {
  BT_GRAPH_MODE_2D = 0,
  BT_GRAPH_MODE_3D = 1
} BtGraphMode;

/* Neighbor mask values written by bt_graph_compute_neighbors(). */
#define BT_NBR_NONE     ((guint8)0u)
#define BT_NBR_NEIGHBOR ((guint8)1u)
#define BT_NBR_FOCUS    ((guint8)2u)

typedef struct {
  gboolean gl_context_ok;
  gboolean is_gles;
  gint gl_major;
  gint gl_minor;
  gboolean shaders_ok;
  gboolean fbo_ok;
  gboolean force_cairo_env;
} BtGraphGlProbe;

typedef struct {
  BtGraphBackend backend;
  gboolean bloom_enabled;
  gboolean mode_3d_available;
  gchar reason[128];
} BtGraphBackendDecision;

typedef struct {
  float x;
  float y;
  float z;
} BtGraphVec3;

/* Selects GL vs Cairo fallback from runtime probe results. Never crashes. */
void bt_graph_select_backend(const BtGraphGlProbe *probe, BtGraphBackendDecision *out);

/* Fills caller-allocated mask[0..mask_len-1] with BT_NBR_* for focus_idx.
 * Zero heap allocations; safe for NULL graph or out-of-range focus_idx. */
void bt_graph_compute_neighbors(const BtGraph *g, gint focus_idx, guint8 *mask, guint mask_len);

/* Computes degree threshold for hub labels so dense graphs stay readable. */
guint bt_graph_hub_degree_threshold(const BtGraph *g);

/* Computes [0.0..1.0] label alpha for a node under current hover/focus/zoom state. */
gdouble bt_graph_label_alpha(const BtGraphNode *nd, guint8 nbr_state, gboolean is_selected,
                             gboolean is_kbd_focus, guint hub_thresh, gdouble zoom,
                             gboolean has_focus, gdouble focus_fade, guint total_nodes);

/* Screen-space greedy label placement (shared by GL and Cairo, zero heap allocs). */
typedef struct {
  guint node_idx;
  float x, y, w, h;
  float alpha;
  guint degree;
  guint8 priority;       /* 5=kbd focus, 4=selected, 3=hovered, 2=neighbor, 1=hub */
  gboolean always_show;  /* focused / selected / hovered: always placed */
  gboolean is_neighbor;  /* counts toward max_neighbors cap */
  gboolean pill;         /* primary pill style */
} BtGraphLabelCand;

typedef struct {
  float cx, cy, r;
  guint node_idx;
} BtGraphCoreObstacle;

/* Default visible label cap at a given zoom (20 at zoom 1.0). */
guint bt_graph_label_cap_for_zoom(gdouble zoom);

/* Sorts cands in-place by (priority desc, degree desc, node_idx asc), skips any
 * non-always_show label overlapping an already placed label or a visible node core,
 * enforces max_labels and max_neighbors, and writes accepted labels to out[]. */
guint bt_graph_place_labels(BtGraphLabelCand *cands, guint cand_count,
                            const BtGraphCoreObstacle *obstacles, guint obstacle_count,
                            guint max_labels, guint max_neighbors,
                            BtGraphLabelCand *out, guint out_cap);

/* Deterministic 3D cortex/spine/brainstem projection derived from BtGraph.
 * Writes MIN(g->nodes->len, max_pts) normalized points into out_pts. */
void bt_graph_compute_3d_layout(const BtGraph *g, BtGraphVec3 *out_pts, guint max_pts);

/* Builds column-major 4x4 perspective orbit MVP matrix for 3D graph mode. */
void bt_graph_build_orbit_mvp(gdouble yaw, gdouble pitch, gdouble dist, gdouble aspect,
                              float mvp_out[16]);

/* Projects a 3D point via mvp into logical screen coordinates (sx, sy) and clip w.
 * Returns FALSE if behind the near plane. */
gboolean bt_graph_project_3d(const float mvp[16], const BtGraphVec3 *pt, gdouble vp_w,
                             gdouble vp_h, gdouble *sx_out, gdouble *sy_out, gdouble *w_out);

G_END_DECLS

#endif
