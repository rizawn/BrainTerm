/* graphview.h — Brain Graph v2 GTK4 widget with unified renderer interface,
 * Cairo 2D fallback (vignette, pre-baked glow sprites, additive/curved edges,
 * drifting dust, label fade), and toolbar controls (§6.C4). */
#ifndef BT_GRAPHVIEW_H
#define BT_GRAPHVIEW_H

#include <cairo.h>
#include <gtk/gtk.h>

#include "graph.h"
#include "graph_state.h"

G_BEGIN_DECLS

typedef struct BtGraphView BtGraphView;

typedef void (*BtGraphOpenNoteFn)(const gchar *relpath, gboolean is_ghost, gpointer user_data);
typedef void (*BtGraphOpenTermFn)(const gchar *rel_dir, gpointer user_data);
typedef void (*BtGraphNoticeFn)(const gchar *message, gpointer user_data);

BtGraphView *bt_graphview_new(BtGraphOpenNoteFn on_open, BtGraphOpenTermFn on_open_term,
                              gpointer user_data);
void bt_graphview_free(BtGraphView *gv);

GtkWidget *bt_graphview_widget(BtGraphView *gv);
void bt_graphview_set_graph(BtGraphView *gv, BtGraph *graph, gboolean animate_growth);
void bt_graphview_set_dark(BtGraphView *gv, gboolean dark);
void bt_graphview_set_quality(BtGraphView *gv, BtGraphQuality quality);
BtGraphQuality bt_graphview_get_quality(const BtGraphView *gv);
void bt_graphview_set_reduce_motion(BtGraphView *gv, gboolean reduce_motion);
gboolean bt_graphview_get_reduce_motion(const BtGraphView *gv);
void bt_graphview_set_notice_cb(BtGraphView *gv, BtGraphNoticeFn on_notice, gpointer user_data);
gboolean bt_graphview_has_graph(const BtGraphView *gv);
gdouble bt_graphview_min_birth_t(const BtGraphView *gv);

/* Pure Cairo draw helper (also callable headless by tests & benches). */
void bt_graph_draw_cairo(const BtGraph *g, cairo_t *cr, int width, int height, gdouble pan_x,
                         gdouble pan_y, gdouble zoom, gint hover_idx, gboolean dark_mode);

G_END_DECLS

#endif
