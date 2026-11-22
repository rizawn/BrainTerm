/* graphview.c — Monochrome ("Ink") 2D Cairo Brain Graph renderer (§6.C4, M-UI §E). */
#include "graphview.h"
#include "theme.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct BtGraphView {
  GtkWidget *root;
  GtkWidget *area;
  BtGraph *graph;

  gdouble pan_x;
  gdouble pan_y;
  gdouble zoom;
  gboolean dark_mode;
  gboolean dragging;
  gdouble drag_start_x, drag_start_y;
  gdouble pan_start_x, pan_start_y;
  gint hover_idx;
  guint anim_id;

  BtGraphOpenNoteFn on_open;
  BtGraphOpenTermFn on_open_term;
  gpointer user_data;
};

static inline void set_gray(cairo_t *cr, guint32 rgb, gdouble alpha)
{
  gdouble r, g, b;
  bt_theme_unpack(rgb, &r, &g, &b);
  if (alpha >= 0.999)
    cairo_set_source_rgb(cr, r, g, b);
  else
    cairo_set_source_rgba(cr, r, g, b, alpha);
}

/* Interim palette (Step 1 shim) — replaced by the full Dusk renderer in Step 5. */
static const struct {
  guint32 surface, hairline, ink, bright, mid, faint;
} PAL = {BT_COL_SURFACE_1, BT_COL_BORDER, BT_COL_TEXT_PRIMARY, BT_COL_TEXT_PRIMARY,
         BT_COL_ACCENT, BT_COL_TEXT_MUTED};

void bt_graph_draw_cairo(const BtGraph *g, cairo_t *cr, int width, int height, gdouble pan_x,
                         gdouble pan_y, gdouble zoom, gint hover_idx, gboolean dark_mode)
{
  (void)dark_mode;
  set_gray(cr, PAL.surface, 1.0);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (g == NULL)
    return;

  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
  cairo_translate(cr, width * 0.5 + pan_x, height * 0.5 + pan_y);
  cairo_scale(cr, zoom, zoom);

  /* World-space viewport bounds for culling. */
  gdouble min_wx = (-width * 0.5 - pan_x) / zoom - 24.0;
  gdouble max_wx = (width * 0.5 - pan_x) / zoom + 24.0;
  gdouble min_wy = (-height * 0.5 - pan_y) / zoom - 24.0;
  gdouble max_wy = (height * 0.5 - pan_y) / zoom + 24.0;

  /* Spine guide line (hairline). */
  set_gray(cr, PAL.hairline, 1.0);
  cairo_set_line_width(cr, 1.5 / zoom);
  cairo_move_to(cr, 0.0, -240.0);
  cairo_line_to(cr, 0.0, 260.0);
  cairo_stroke(cr);

  gboolean lod_collapse = (g->nodes->len > 2000 && zoom < 0.6);

  /* Edges: 8% alpha ink lines, AA off on 1px hairlines for 60+ fps. */
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_line_width(cr, 1.0 / zoom);
  set_gray(cr, PAL.ink, 0.09);
  for (guint i = 0; i < g->edges->len; i++) {
    const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
    if (lod_collapse && e->kind == BT_EDGE_TAGGED)
      continue;
    const BtGraphNode *a = g_ptr_array_index(g->nodes, e->src_idx);
    const BtGraphNode *b = g_ptr_array_index(g->nodes, e->dst_idx);
    if (!a->visible || !b->visible)
      continue;
    gdouble ax = a->x * a->birth_t, ay = 260.0 + (a->y - 260.0) * a->birth_t;
    gdouble bx = b->x * b->birth_t, by = 260.0 + (b->y - 260.0) * b->birth_t;
    if ((ax < min_wx && bx < min_wx) || (ax > max_wx && bx > max_wx) ||
        (ay < min_wy && by < min_wy) || (ay > max_wy && by > max_wy))
      continue;
    cairo_move_to(cr, ax, ay);
    cairo_line_to(cr, bx, by);
  }
  cairo_stroke(cr);

  /* Nodes: strictly monochrome ink shapes (§E.10–E.11). */
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
  cairo_set_tolerance(cr, 0.5);
  const gdouble dashes[] = {3.0, 3.0};
  for (guint i = 0; i < g->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (!nd->visible)
      continue;
    gdouble nx = nd->x * nd->birth_t;
    gdouble ny = 260.0 + (nd->y - 260.0) * nd->birth_t;

    if (nd->birth_t < 0.99) {
      /* Monochrome fade-in halo (no colored glow). */
      set_gray(cr, PAL.bright, 0.25 * (1.0 - nd->birth_t));
      cairo_arc(cr, nx, ny, nd->radius * 2.0, 0, 2.0 * M_PI);
      cairo_fill(cr);
    }

    if (nd->kind == BT_NODE_HUMAN_NOTE) {
      set_gray(cr, PAL.ink, 1.0);
      cairo_arc(cr, nx, ny, nd->radius, 0, 2.0 * M_PI);
      cairo_fill(cr);
    } else if (nd->kind == BT_NODE_AI_NOTE) {
      set_gray(cr, PAL.mid, 1.0);
      cairo_set_line_width(cr, 1.6 / zoom);
      cairo_arc(cr, nx, ny, nd->radius, 0, 2.0 * M_PI);
      cairo_stroke(cr);
    } else if (nd->kind == BT_NODE_TAG) {
      set_gray(cr, PAL.faint, 1.0);
      cairo_set_line_width(cr, 1.2 / zoom);
      cairo_arc(cr, nx, ny, nd->radius, 0, 2.0 * M_PI);
      cairo_stroke(cr);
    } else if (nd->kind == BT_NODE_SESSION) {
      set_gray(cr, PAL.ink, 1.0);
      cairo_rectangle(cr, nx - nd->radius, ny - nd->radius, nd->radius * 2.0, nd->radius * 2.0);
      cairo_fill(cr);
    } else if (nd->kind == BT_NODE_IMPORT_ROOT) {
      set_gray(cr, PAL.ink, 1.0);
      cairo_set_line_width(cr, 1.6 / zoom);
      cairo_rectangle(cr, nx - nd->radius, ny - nd->radius, nd->radius * 2.0, nd->radius * 2.0);
      cairo_stroke(cr);
    } else if (nd->kind == BT_NODE_GHOST) {
      set_gray(cr, nd->broken ? PAL.faint : PAL.mid, nd->broken ? 0.35 : 0.85);
      cairo_set_dash(cr, dashes, 2, 0);
      cairo_set_line_width(cr, 1.3 / zoom);
      cairo_arc(cr, nx, ny, nd->radius, 0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);
    }

    if ((gint)i == hover_idx) {
      set_gray(cr, PAL.bright, 1.0);
      cairo_set_line_width(cr, 1.8 / zoom);
      cairo_arc(cr, nx, ny, nd->radius + 3.0, 0, 2.0 * M_PI);
      cairo_stroke(cr);
    }
  }

  /* Hover label (monochrome ink, GRAY font AA so Windows ClearType never adds RGB fringes). */
  if (hover_idx >= 0 && (guint)hover_idx < g->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, (guint)hover_idx);
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_set_font_options(cr, fo);
    cairo_font_options_destroy(fo);
    set_gray(cr, PAL.bright, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0 / zoom);
    cairo_move_to(cr, nd->x + nd->radius + 5.0, nd->y + 4.0);
    cairo_show_text(cr, nd->label);
  }

  cairo_restore(cr);
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
  (void)area;
  BtGraphView *gv = ud;
  bt_graph_draw_cairo(gv->graph, cr, w, h, gv->pan_x, gv->pan_y, gv->zoom, gv->hover_idx,
                      gv->dark_mode);
}

static void screen_to_world(BtGraphView *gv, gdouble sx, gdouble sy, gdouble *wx, gdouble *wy)
{
  int w = gtk_widget_get_width(gv->area);
  int h = gtk_widget_get_height(gv->area);
  *wx = (sx - w * 0.5 - gv->pan_x) / gv->zoom;
  *wy = (sy - h * 0.5 - gv->pan_y) / gv->zoom;
}

static void on_click_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g;
  BtGraphView *gv = ud;
  if (n_press != 1 || gv->graph == NULL)
    return;
  gdouble wx = 0, wy = 0;
  screen_to_world(gv, x, y, &wx, &wy);
  gint hit = bt_graph_pick(gv->graph, wx, wy, 14.0 / gv->zoom);
  if (hit >= 0) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    if (nd->relpath != NULL && !nd->broken && gv->on_open != NULL)
      gv->on_open(nd->relpath, nd->kind == BT_NODE_GHOST, gv->user_data);
    return;
  }
  gv->dragging = TRUE;
  gv->drag_start_x = x;
  gv->drag_start_y = y;
  gv->pan_start_x = gv->pan_x;
  gv->pan_start_y = gv->pan_y;
}

static void on_click_released(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g; (void)n_press; (void)x; (void)y;
  ((BtGraphView *)ud)->dragging = FALSE;
}

static void on_right_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g;
  BtGraphView *gv = ud;
  if (n_press != 1 || gv->graph == NULL || gv->on_open_term == NULL)
    return;
  gdouble wx = 0, wy = 0;
  screen_to_world(gv, x, y, &wx, &wy);
  gint hit = bt_graph_pick(gv->graph, wx, wy, 14.0 / gv->zoom);
  if (hit >= 0) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    if (nd->relpath != NULL && !nd->broken)
      gv->on_open_term(nd->relpath, gv->user_data);
  }
}

static void on_motion(GtkEventControllerMotion *c, gdouble x, gdouble y, gpointer ud)
{
  (void)c;
  BtGraphView *gv = ud;
  if (gv->dragging) {
    gv->pan_x = gv->pan_start_x + (x - gv->drag_start_x);
    gv->pan_y = gv->pan_start_y + (y - gv->drag_start_y);
    gtk_widget_queue_draw(gv->area);
    return;
  }
  if (gv->graph != NULL) {
    gdouble wx = 0, wy = 0;
    screen_to_world(gv, x, y, &wx, &wy);
    gint hit = bt_graph_pick(gv->graph, wx, wy, 14.0 / gv->zoom);
    if (hit != gv->hover_idx) {
      gv->hover_idx = hit;
      gtk_widget_queue_draw(gv->area);
    }
  }
}

static gboolean on_scroll(GtkEventControllerScroll *c, gdouble dx, gdouble dy, gpointer ud)
{
  (void)c; (void)dx;
  BtGraphView *gv = ud;
  gv->zoom = CLAMP(gv->zoom * (dy < 0 ? 1.12 : 0.89), 0.2, 4.0);
  gtk_widget_queue_draw(gv->area);
  return TRUE;
}

static gboolean on_anim_tick(gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->graph == NULL || gv->area == NULL || !gtk_widget_get_mapped(gv->area)) {
    gv->anim_id = 0;
    return G_SOURCE_REMOVE;
  }
  gboolean active = FALSE;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (nd->birth_t < 1.0) {
      nd->birth_t = MIN(nd->birth_t + 0.08, 1.0);
      active = TRUE;
    }
  }
  gtk_widget_queue_draw(gv->area);
  if (!active) {
    gv->anim_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean graph_has_pending_birth(const BtGraph *g)
{
  if (g == NULL)
    return FALSE;
  for (guint i = 0; i < g->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->birth_t < 1.0)
      return TRUE;
  }
  return FALSE;
}

static void on_area_map(GtkWidget *area, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->graph != NULL && graph_has_pending_birth(gv->graph) && gv->anim_id == 0)
    gv->anim_id = g_timeout_add(16, on_anim_tick, gv);
  gtk_widget_queue_draw(area);
}

static void on_area_destroy(GtkWidget *area, gpointer ud)
{
  (void)area;
  BtGraphView *gv = ud;
  if (gv->anim_id != 0) {
    g_source_remove(gv->anim_id);
    gv->anim_id = 0;
  }
  gv->area = NULL;
  gv->root = NULL;
}

BtGraphView *bt_graphview_new(BtGraphOpenNoteFn on_open, BtGraphOpenTermFn on_open_term,
                              gpointer user_data)
{
  BtGraphView *gv = g_new0(BtGraphView, 1);
  gv->zoom = 1.0;
  gv->dark_mode = TRUE;
  gv->hover_idx = -1;
  gv->on_open = on_open;
  gv->on_open_term = on_open_term;
  gv->user_data = user_data;

  gv->area = gtk_drawing_area_new();
  gtk_widget_set_name(gv->area, "bt-graph-area");
  gtk_widget_set_hexpand(gv->area, TRUE);
  gtk_widget_set_vexpand(gv->area, TRUE);
  gtk_widget_set_size_request(gv->area, 200, 160);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(gv->area), on_draw, gv, NULL);
  g_signal_connect(gv->area, "map", G_CALLBACK(on_area_map), gv);
  g_signal_connect(gv->area, "destroy", G_CALLBACK(on_area_destroy), gv);

  GtkGesture *lclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lclick), GDK_BUTTON_PRIMARY);
  g_signal_connect(lclick, "pressed", G_CALLBACK(on_click_pressed), gv);
  g_signal_connect(lclick, "released", G_CALLBACK(on_click_released), gv);
  gtk_widget_add_controller(gv->area, GTK_EVENT_CONTROLLER(lclick));

  GtkGesture *rclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect(rclick, "pressed", G_CALLBACK(on_right_click), gv);
  gtk_widget_add_controller(gv->area, GTK_EVENT_CONTROLLER(rclick));

  GtkEventController *mot = gtk_event_controller_motion_new();
  g_signal_connect(mot, "motion", G_CALLBACK(on_motion), gv);
  gtk_widget_add_controller(gv->area, mot);

  GtkEventController *scr = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scr, "scroll", G_CALLBACK(on_scroll), gv);
  gtk_widget_add_controller(gv->area, scr);

  gv->root = gv->area;
  return gv;
}

void bt_graphview_free(BtGraphView *gv)
{
  if (gv == NULL)
    return;
  if (gv->anim_id != 0)
    g_source_remove(gv->anim_id);
  bt_graph_free(gv->graph);
  g_free(gv);
}

GtkWidget *bt_graphview_widget(BtGraphView *gv)
{
  return gv->root;
}

void bt_graphview_set_graph(BtGraphView *gv, BtGraph *graph, gboolean animate_growth)
{
  g_return_if_fail(gv != NULL);
  bt_graph_free(gv->graph);
  gv->graph = graph;
  gv->hover_idx = -1;
  if (gv->graph != NULL && animate_growth) {
    for (guint i = 0; i < gv->graph->nodes->len; i++)
      ((BtGraphNode *)g_ptr_array_index(gv->graph->nodes, i))->birth_t = 0.05;
    /* Start animation immediately only if mapped; unmapped views resume in on_area_map. */
    if (gv->area != NULL && gtk_widget_get_mapped(gv->area) && gv->anim_id == 0)
      gv->anim_id = g_timeout_add(16, on_anim_tick, gv);
  }
  if (gv->area != NULL)
    gtk_widget_queue_draw(gv->area);
}

void bt_graphview_set_dark(BtGraphView *gv, gboolean dark)
{
  if (gv == NULL)
    return;
  gv->dark_mode = dark;
  if (gv->area != NULL)
    gtk_widget_queue_draw(gv->area);
}

gboolean bt_graphview_has_graph(const BtGraphView *gv)
{
  return gv != NULL && gv->graph != NULL;
}

gdouble bt_graphview_min_birth_t(const BtGraphView *gv)
{
  if (gv == NULL || gv->graph == NULL || gv->graph->nodes->len == 0)
    return 1.0;
  gdouble min_b = 1.0;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (nd->birth_t < min_b)
      min_b = nd->birth_t;
  }
  return min_b;
}
