/* graph_timeline.h — Pure-C (GTK-free) Timeline layout, beeswarm/kept packing,
 * date parsing, and ledger event model (Phase 4, mindcorpus-inspired). */
#ifndef BT_GRAPH_TIMELINE_H
#define BT_GRAPH_TIMELINE_H

#include <glib.h>

#include "graph.h"

G_BEGIN_DECLS

typedef enum {
  BT_TL_LANE_FOLDER = 0,
  BT_TL_LANE_TYPE = 1
} BtTimelineLaneBy;

typedef enum {
  BT_LEDGER_CHECKPOINT = 0,
  BT_LEDGER_SESSION = 1,
  BT_LEDGER_IMPORT = 2,
  BT_LEDGER_AI_EDIT = 3
} BtLedgerKind;

typedef struct {
  guint node_idx;
  gint64 ts;          /* resolved unix timestamp (sec) */
  guint lane_idx;
  float org_x, org_y; /* Organic wavy beeswarm coords (screen px) */
  float kept_x, kept_y;/* Kept tidy grid coords inside framed box (screen px) */
  float radius;
  gboolean valid;
} BtTimelineDot;

typedef struct {
  gint64 ts;
  BtLedgerKind kind;
  gchar label[48];
  guint32 color_rgb;
} BtLedgerEvent;

typedef struct {
  BtTimelineDot *dots;
  guint dot_count;
  gint64 min_ts;
  gint64 max_ts;
  guint lane_count;
  gchar **lane_names;
  guint32 *lane_colors;
  BtLedgerEvent *events;
  guint event_count;
} BtTimelineLayout;

/* Parses ISO-8601 / YYYY-MM-DD date string to unix seconds (UTC). Returns 0 on failure. */
gint64 bt_timeline_parse_date(const gchar *date_str);

/* Computes deterministic Organic (wavy lane beeswarm) and Kept (framed grid) positions
 * for all visible notes in graph within [x0..x1, y0..y1] for time window [t_min..t_max].
 * Guarantees no two dots overlap (dist >= 2*r + 1px) in both modes. */
BtTimelineLayout *bt_timeline_build(const BtGraph *g, BtTimelineLaneBy lane_by,
                                    float x0, float x1, float y0, float y1,
                                    gint64 view_min_ts, gint64 view_max_ts);

void bt_timeline_free(BtTimelineLayout *tl);

G_END_DECLS

#endif
