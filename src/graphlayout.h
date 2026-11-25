/* graphlayout.h — Deterministic, GTK-free 2D force-directed layout engine
 * for BrainTerm ("Clean" Obsidian-style layout: spatial grid repulsion,
 * connected-component packing, halo ring for loose/tiny groups, guaranteed
 * disc de-overlap, warm-start stability, and node pinning). */
#ifndef BT_GRAPHLAYOUT_H
#define BT_GRAPHLAYOUT_H

#include <glib.h>

#include "graph.h"

G_BEGIN_DECLS

typedef struct BtGraphLayoutOpts {
  guint32 seed;          /* default fixed seed (0x5A17C0DEu), deterministic */
  guint max_iterations;  /* headless run: default 300 */
  float link_distance;   /* default 30.0 px at zoom 1 */
  float gravity;         /* default 0.03 */
  float collision_pad;   /* default 1.5 px */
  gboolean warm_start;   /* preserve existing finite node x,y and reheat gently */
  gdouble time_cap_ms;   /* 0 = no cap */
} BtGraphLayoutOpts;

typedef struct BtGraphLayoutState BtGraphLayoutState;

/* Populates BtGraphLayoutOpts with tuned defaults. */
void bt_graph_layout_default_opts(BtGraphLayoutOpts *o);

/* Single source of truth for zoom-1 node core radius (shared by layout, Cairo, GL, picking). */
float bt_graph_node_radius(const BtGraph *g, guint node_index);

/* Helper computing radius directly from node kind and visible degree. */
float bt_graph_node_radius_for_degree(BtNodeKind kind, guint vis_degree);

/* Begins an incremental or headless 2D layout over visible nodes of g. */
BtGraphLayoutState *bt_graph_layout_begin(BtGraph *g, const BtGraphLayoutOpts *o);

/* Runs up to n simulation iterations or until budget_ms (0 = unlimited) elapses.
 * Returns TRUE while still moving, FALSE once converged or iteration cap reached. */
gboolean bt_graph_layout_step(BtGraphLayoutState *s, guint n, gdouble budget_ms);

/* Runs final collision relaxation (12..40 passes), component de-overlap, and
 * writes final x, y, radius back into g->nodes. */
void bt_graph_layout_finish(BtGraphLayoutState *s);

void bt_graph_layout_free(BtGraphLayoutState *s);

/* Convenience headless runner: begin -> step until done -> finish -> free. */
void bt_graph_layout_run(BtGraph *g, const BtGraphLayoutOpts *o);

/* Pin / unpin a node index in an active layout state (pinned nodes never move). */
void bt_graph_layout_set_pinned(BtGraphLayoutState *s, guint node_index, gboolean pinned);
void bt_graph_layout_set_pos(BtGraphLayoutState *s, guint node_index, float x, float y);
void bt_graph_layout_reheat(BtGraphLayoutState *s, float alpha);

/* Telemetry from a completed or in-progress layout state. */
guint bt_graph_layout_iterations_run(const BtGraphLayoutState *s);
guint bt_graph_layout_nan_resets(const BtGraphLayoutState *s);
guint bt_graph_layout_remaining_overlaps(const BtGraphLayoutState *s);

G_END_DECLS

#endif
