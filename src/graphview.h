/* graphview.h — 2D Cairo Brain Graph GTK4 widget with pan/zoom, filters,
 * ghost creation, and brainstem->cortex growth animation (§6.C4). */
#ifndef BT_GRAPHVIEW_H
#define BT_GRAPHVIEW_H

#include <cairo.h>
#include <gtk/gtk.h>

#include "graph.h"

G_BEGIN_DECLS

typedef struct BtGraphView BtGraphView;

typedef void (*BtGraphOpenNoteFn)(const gchar *relpath, gboolean is_ghost, gpointer user_data);
typedef void (*BtGraphOpenTermFn)(const gchar *rel_dir, gpointer user_data);

BtGraphView *bt_graphview_new(BtGraphOpenNoteFn on_open, BtGraphOpenTermFn on_open_term,
                              gpointer user_data);
void bt_graphview_free(BtGraphView *gv);

GtkWidget *bt_graphview_widget(BtGraphView *gv);
void bt_graphview_set_graph(BtGraphView *gv, BtGraph *graph, gboolean animate_growth);
void bt_graphview_set_dark(BtGraphView *gv, gboolean dark);
gboolean bt_graphview_has_graph(const BtGraphView *gv);
gdouble bt_graphview_min_birth_t(const BtGraphView *gv);

/* Pure Cairo draw helper (also callable headless by bench_graph & test_graph_mono).
 * Strictly monochrome (R == G == B for every rendered pixel). */
void bt_graph_draw_cairo(const BtGraph *g, cairo_t *cr, int width, int height, gdouble pan_x,
                         gdouble pan_y, gdouble zoom, gint hover_idx, gboolean dark_mode);

G_END_DECLS

#endif
