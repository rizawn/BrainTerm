/* graphview.h — Brain Graph v2 GTK4 widget with unified renderer interface,
 * Cairo 2D fallback (vignette, pre-baked glow sprites, additive/curved edges,
 * drifting dust, label fade), and toolbar controls (§6.C4). */
#ifndef BT_GRAPHVIEW_H
#define BT_GRAPHVIEW_H

#include <cairo.h>
#include <gtk/gtk.h>

#include "graph.h"
#include "graph_state.h"
#include "ui/graphcapture.h"

G_BEGIN_DECLS

typedef struct BtGraphView BtGraphView;

typedef void (*BtGraphOpenNoteFn)(const gchar *relpath, gboolean is_ghost, gpointer user_data);
typedef void (*BtGraphOpenTermFn)(const gchar *rel_dir, gpointer user_data);
typedef void (*BtGraphRenameNoteFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtGraphSelectNoteFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtGraphNoticeFn)(const gchar *message, gpointer user_data);
typedef void (*BtGraphPrefsChangedFn)(gboolean show_tags, gboolean show_ghosts, gpointer user_data);

BtGraphView *bt_graphview_new(BtGraphOpenNoteFn on_open, BtGraphOpenTermFn on_open_term,
                              gpointer user_data);
void bt_graphview_free(BtGraphView *gv);

GtkWidget *bt_graphview_widget(BtGraphView *gv);
void bt_graphview_set_vault_root(BtGraphView *gv, const gchar *vault_root);
void bt_graphview_set_graph(BtGraphView *gv, BtGraph *graph, gboolean animate_growth);
void bt_graphview_set_dark(BtGraphView *gv, gboolean dark);
void bt_graphview_set_quality(BtGraphView *gv, BtGraphQuality quality);
BtGraphQuality bt_graphview_get_quality(const BtGraphView *gv);
void bt_graphview_set_mode(BtGraphView *gv, BtGraphMode mode);
BtGraphMode bt_graphview_get_mode(const BtGraphView *gv);
void bt_graphview_set_pulse(BtGraphView *gv, gboolean enabled);
gboolean bt_graphview_get_pulse(const BtGraphView *gv);
void bt_graphview_set_reduce_motion(BtGraphView *gv, gboolean reduce_motion);
gboolean bt_graphview_get_reduce_motion(const BtGraphView *gv);
void bt_graphview_set_show_tags(BtGraphView *gv, gboolean show_tags);
gboolean bt_graphview_get_show_tags(const BtGraphView *gv);
void bt_graphview_set_show_ghosts(BtGraphView *gv, gboolean show_ghosts);
gboolean bt_graphview_get_show_ghosts(const BtGraphView *gv);
void bt_graphview_set_show_links(BtGraphView *gv, gboolean show_links);
gboolean bt_graphview_get_show_links(const BtGraphView *gv);
void bt_graphview_set_select_cb(BtGraphView *gv, BtGraphSelectNoteFn on_select, gpointer user_data);
void bt_graphview_select_relpath(BtGraphView *gv, const gchar *relpath);
void bt_graphview_set_prefs_cb(BtGraphView *gv, BtGraphPrefsChangedFn on_prefs, gpointer user_data);
void bt_graphview_set_debug_overlay(BtGraphView *gv, gboolean enabled);
gboolean bt_graphview_get_debug_overlay(const BtGraphView *gv);
void bt_graphview_toggle_debug_overlay(BtGraphView *gv);
gboolean bt_graphview_is_capture_ready(const BtGraphView *gv, const BtGraphCaptureSpec *spec);
void bt_graphview_apply_capture_state(BtGraphView *gv, const BtGraphCaptureSpec *spec);
gboolean bt_graphview_is_screenshot_ready(const BtGraphView *gv);
void bt_graphview_prepare_screenshot(BtGraphView *gv);
gboolean bt_graphview_capture_evidence(BtGraphView *gv, GtkWindow *win, const gchar *out_dir,
                                       const BtGraphCaptureSpec *spec, const gchar *build_stamp,
                                       GError **error);
void bt_graphview_set_notice_cb(BtGraphView *gv, BtGraphNoticeFn on_notice, gpointer user_data);
void bt_graphview_set_rename_cb(BtGraphView *gv, BtGraphRenameNoteFn on_rename, gpointer user_data);
gboolean bt_graphview_has_graph(const BtGraphView *gv);
gdouble bt_graphview_min_birth_t(const BtGraphView *gv);

/* Pure Cairo draw helper (also callable headless by tests & benches). */
void bt_graph_draw_cairo(const BtGraph *g, cairo_t *cr, int width, int height, gdouble pan_x,
                         gdouble pan_y, gdouble zoom, gint hover_idx, gboolean dark_mode);

G_END_DECLS

#endif
