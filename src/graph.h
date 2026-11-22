/* graph.h — Brain Graph model, deterministic spine/brainstem/cortex layout,
 * ghost nodes, filters, and node picking (§6.C1–§6.C4). */
#ifndef BT_GRAPH_H
#define BT_GRAPH_H

#include <glib.h>

#include "index.h"
#include "sessions.h"
#include "vault.h"

G_BEGIN_DECLS

typedef enum {
  BT_NODE_HUMAN_NOTE,  /* filled circle, size ~ degree */
  BT_NODE_AI_NOTE,     /* hollow circle, size ~ degree (source: ai | import) */
  BT_NODE_TAG,         /* small colored dot */
  BT_NODE_SESSION,     /* square (brainstem) */
  BT_NODE_IMPORT_ROOT, /* large square (brainstem) */
  BT_NODE_GHOST        /* dashed circle (missing wiki-link target) */
} BtNodeKind;

typedef enum {
  BT_EDGE_LINK,
  BT_EDGE_SIMILAR,
  BT_EDGE_TEMPORAL,
  BT_EDGE_TAGGED
} BtEdgeKind;

typedef struct {
  gchar *id;
  gchar *label;
  gchar *relpath;   /* non-NULL for notes; target path for valid ghosts */
  gchar *source;    /* "human" | "ai" | "import" | NULL */
  gchar *dom_tag;   /* dominant tag for cortex clustering & hue */
  BtNodeKind kind;
  gint64 created;
  guint degree;
  gboolean broken;  /* TRUE if ghost escapes vault (§8.3: dimmed, unclickable) */
  gboolean visible; /* filter result */

  gdouble x, y;     /* resting layout coordinates */
  gdouble hue;      /* [0, 360) */
  gdouble radius;
  gdouble birth_t;  /* [0..1] growth animation progress */
} BtGraphNode;

typedef struct {
  guint src_idx;
  guint dst_idx;
  BtEdgeKind kind;
  gdouble weight;
} BtGraphEdge;

typedef struct {
  const gchar *source_filter; /* NULL or "" = all; else "human"|"ai"|"import" */
  const gchar *tag_filter;    /* NULL or "" = all */
  gint64 min_created;
  gint64 max_created;
} BtGraphFilter;

typedef struct {
  GPtrArray *nodes; /* BtGraphNode* */
  GArray *edges;    /* BtGraphEdge */
  guint note_node_count;
  guint ghost_node_count;
} BtGraph;

/* Builds the full Brain Graph from SQLite index + optional sessions store.
 * Guarantees note_node_count == number of indexed .md notes (§6.C2). */
BtGraph *bt_graph_build(const BtVault *vault, BtIndex *index, const BtSessionStore *sessions);
void bt_graph_free(BtGraph *g);

/* Deterministic brain layout: forced brainstem (bottom), forced spine (center
 * chronological axis), force-relaxed cortex clusters radial from spine. */
void bt_graph_layout(BtGraph *g, guint relax_iters);

void bt_graph_apply_filter(BtGraph *g, const BtGraphFilter *filter);

/* Returns index of closest visible node within hit_radius, or -1. */
gint bt_graph_pick(const BtGraph *g, gdouble wx, gdouble wy, gdouble hit_radius);

G_END_DECLS

#endif
