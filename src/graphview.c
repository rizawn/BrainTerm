/* graphview.c — Dusk 2D Cairo Brain Graph renderer:
 *   - Faint dot grid on surface-1, HiDPI Pango labels, antialiased Cairo.
 *   - Nodes sized by degree, colored by type (note / AI / tag / folder / session).
 *   - Soft radial hover glow; hovering highlights connected edges & dims others to 25%.
 *   - Auto-fit & center on load, overlay zoom/fit/reset controls, mini legend, empty state.
 *   - Frame-synced animations via gtk_widget_add_tick_callback (stops immediately when idle). */
#include "graphview.h"
#include "theme.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct BtGraphView {
  GtkWidget *root;    /* GtkStack: "canvas" | "empty" */
  GtkWidget *overlay; /* GtkOverlay wrapping area + controls + legend */
  GtkWidget *area;    /* GtkDrawingArea ("bt-graph-area") */
  BtGraph *graph;

  gdouble pan_x, pan_y, zoom;
  gdouble target_pan_x, target_pan_y, target_zoom;
  gboolean animating_cam;
  gboolean needs_fit;
  gboolean dark_mode;

  gboolean dragging;
  gdouble drag_start_x, drag_start_y;
  gdouble pan_start_x, pan_start_y;

  gint hover_idx;
  gdouble hover_fade; /* 0..1 smooth highlight transition */
  guint tick_id;

  BtGraphOpenNoteFn on_open;
  BtGraphOpenTermFn on_open_term;
  gpointer user_data;
};

/* ----------------------------------------------------- color helpers -- */

static inline void set_tok(cairo_t *cr, guint32 rgb, gdouble alpha)
{
  gdouble r, g, b;
  bt_theme_unpack(rgb, &r, &g, &b);
  if (alpha >= 0.999)
    cairo_set_source_rgb(cr, r, g, b);
  else
    cairo_set_source_rgba(cr, r, g, b, alpha);
}

static guint32 node_color(BtNodeKind k)
{
  switch (k) {
  case BT_NODE_HUMAN_NOTE:  return BT_COL_NODE_NOTE;
  case BT_NODE_AI_NOTE:     return BT_COL_NODE_AI;
  case BT_NODE_TAG:         return BT_COL_NODE_TAG;
  case BT_NODE_IMPORT_ROOT: return BT_COL_NODE_FOLDER;
  case BT_NODE_SESSION:     return BT_COL_TEXT_SECONDARY;
  case BT_NODE_GHOST:
  default:                  return BT_COL_TEXT_MUTED;
  }
}

static void draw_rrect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r)
{
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0.0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI * 0.5);
  cairo_arc(cr, x + r, y + h - r, r, M_PI * 0.5, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
  cairo_close_path(cr);
}

static inline void node_pos(const BtGraphNode *nd, gdouble *x, gdouble *y)
{
  if (nd->birth_t >= 0.999) {
    *x = nd->x;
    *y = nd->y;
  } else {
    gdouble t = 1.0 - pow(1.0 - nd->birth_t, 3.0); /* ease-out cubic */
    *x = nd->x * t;
    *y = nd->y * t;
  }
}

static void draw_dot_grid(cairo_t *cr, int width, int height, gdouble pan_x, gdouble pan_y,
                          gdouble zoom)
{
  gdouble step = 28.0 * CLAMP(zoom, 0.5, 1.5);
  if (step < 14.0)
    step = 14.0;
  gdouble ox = fmod(width * 0.5 + pan_x, step);
  gdouble oy = fmod(height * 0.5 + pan_y, step);
  if (ox < 0.0) ox += step;
  if (oy < 0.0) oy += step;

  set_tok(cr, BT_COL_BORDER, 0.55);
  for (gdouble y = oy; y < (gdouble)height; y += step) {
    for (gdouble x = ox; x < (gdouble)width; x += step)
      cairo_rectangle(cr, floor(x), floor(y), 1.5, 1.5);
  }
  cairo_fill(cr);
}

static void draw_graph_internal(const BtGraph *g, cairo_t *cr, int width, int height,
                                gdouble pan_x, gdouble pan_y, gdouble zoom,
                                gint hover_idx, gdouble hover_fade)
{
  /* Card background = surface-1. */
  set_tok(cr, BT_COL_SURFACE_1, 1.0);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (g == NULL || g->nodes->len == 0)
    return;

  gboolean heavy = (g->nodes->len > 400);
  if (!heavy)
    draw_dot_grid(cr, width, height, pan_x, pan_y, zoom);

  cairo_save(cr);
  cairo_set_antialias(cr, heavy ? CAIRO_ANTIALIAS_FAST : CAIRO_ANTIALIAS_GOOD);
  cairo_translate(cr, width * 0.5 + pan_x, height * 0.5 + pan_y);
  cairo_scale(cr, zoom, zoom);

  gdouble min_wx = (-width * 0.5 - pan_x) / zoom - 32.0;
  gdouble max_wx = (width * 0.5 - pan_x) / zoom + 32.0;
  gdouble min_wy = (-height * 0.5 - pan_y) / zoom - 32.0;
  gdouble max_wy = (height * 0.5 - pan_y) / zoom + 32.0;

  /* Intentional subtle session dock line only when session/import nodes exist. */
  gdouble stem_min_x = 1e9, stem_max_x = -1e9;
  gboolean has_stem = FALSE;
  if (!heavy) {
    for (guint i = 0; i < g->nodes->len; i++) {
      const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      if (!nd->visible)
        continue;
      if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT) {
        has_stem = TRUE;
        if (nd->x < stem_min_x) stem_min_x = nd->x;
        if (nd->x > stem_max_x) stem_max_x = nd->x;
      }
    }
    if (has_stem) {
      gdouble pad = 36.0;
      set_tok(cr, BT_COL_BORDER_STRONG, 0.55);
      cairo_set_line_width(cr, 1.0 / zoom);
      cairo_move_to(cr, stem_min_x - pad, 238.0);
      cairo_line_to(cr, stem_max_x + pad, 238.0);
      cairo_stroke(cr);
    }
  }

  /* Neighbor mask when hovering a node. */
  gboolean has_hover = (hover_idx >= 0 && (guint)hover_idx < g->nodes->len);
  gdouble dim_factor = has_hover ? (1.0 - 0.75 * hover_fade) : 1.0; /* dims to 25% */
  g_autofree gboolean *adj = NULL;
  if (has_hover) {
    adj = g_new0(gboolean, g->nodes->len);
    adj[hover_idx] = TRUE;
    for (guint i = 0; i < g->edges->len; i++) {
      const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
      if ((gint)e->src_idx == hover_idx)
        adj[e->dst_idx] = TRUE;
      else if ((gint)e->dst_idx == hover_idx)
        adj[e->src_idx] = TRUE;
    }
  }

  gboolean lod_collapse = (g->nodes->len > 2000 && zoom < 0.6);

  /* Pass 1: base (non-highlighted) edges — single batched stroke for 60 fps. */
  cairo_set_antialias(cr, heavy ? CAIRO_ANTIALIAS_NONE : CAIRO_ANTIALIAS_GOOD);
  cairo_set_line_width(cr, 1.0 / zoom);
  set_tok(cr, BT_COL_TEXT_SECONDARY, 0.18 * dim_factor);
  for (guint i = 0; i < g->edges->len; i++) {
    const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
    if (lod_collapse && e->kind == BT_EDGE_TAGGED)
      continue;
    if (has_hover && ((gint)e->src_idx == hover_idx || (gint)e->dst_idx == hover_idx))
      continue;
    const BtGraphNode *a = g_ptr_array_index(g->nodes, e->src_idx);
    const BtGraphNode *b = g_ptr_array_index(g->nodes, e->dst_idx);
    if (!a->visible || !b->visible)
      continue;
    gdouble ax, ay, bx, by;
    node_pos(a, &ax, &ay);
    node_pos(b, &bx, &by);
    if ((ax < min_wx && bx < min_wx) || (ax > max_wx && bx > max_wx) ||
        (ay < min_wy && by < min_wy) || (ay > max_wy && by > max_wy))
      continue;
    cairo_move_to(cr, ax, ay);
    cairo_line_to(cr, bx, by);
  }
  cairo_stroke(cr);

  /* Pass 1b: highlighted connected edges on hover. */
  if (has_hover) {
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    cairo_set_line_width(cr, 1.6 / zoom);
    set_tok(cr, BT_COL_ACCENT, 0.25 + 0.60 * hover_fade);
    for (guint i = 0; i < g->edges->len; i++) {
      const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
      if ((gint)e->src_idx != hover_idx && (gint)e->dst_idx != hover_idx)
        continue;
      const BtGraphNode *a = g_ptr_array_index(g->nodes, e->src_idx);
      const BtGraphNode *b = g_ptr_array_index(g->nodes, e->dst_idx);
      if (!a->visible || !b->visible)
        continue;
      gdouble ax, ay, bx, by;
      node_pos(a, &ax, &ay);
      node_pos(b, &bx, &by);
      cairo_move_to(cr, ax, ay);
      cairo_line_to(cr, bx, by);
    }
    cairo_stroke(cr);
  }

  /* Pass 2: nodes sized by connection count, colored by type. */
  cairo_set_antialias(cr, heavy ? CAIRO_ANTIALIAS_FAST : CAIRO_ANTIALIAS_GOOD);
  if (heavy)
    cairo_set_tolerance(cr, 0.5);
  const gdouble dashes[] = {3.0 / zoom, 3.0 / zoom};

  for (guint i = 0; i < g->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (!nd->visible)
      continue;
    gdouble nx, ny;
    node_pos(nd, &nx, &ny);
    if (nx < min_wx || nx > max_wx || ny < min_wy || ny > max_wy)
      continue;

    gboolean is_hov = ((gint)i == hover_idx);
    gboolean is_nbr = (adj != NULL && adj[i]);
    gdouble alpha = (has_hover && !is_nbr) ? dim_factor : 1.0;
    guint32 col = node_color(nd->kind);
    gdouble rad = nd->radius;

    /* Soft radial glow on hover or AI nodes. */
    if (!heavy && (is_hov || (nd->kind == BT_NODE_AI_NOTE && alpha > 0.5))) {
      gdouble gr = is_hov ? rad * (2.2 + 1.1 * hover_fade) : rad * 2.1;
      gdouble ga = is_hov ? (0.42 * hover_fade) : (0.16 * alpha);
      gdouble cr_r, cr_g, cr_b;
      bt_theme_unpack(is_hov ? BT_COL_ACCENT : col, &cr_r, &cr_g, &cr_b);
      cairo_pattern_t *pat = cairo_pattern_create_radial(nx, ny, rad * 0.4, nx, ny, gr);
      cairo_pattern_add_color_stop_rgba(pat, 0.0, cr_r, cr_g, cr_b, ga);
      cairo_pattern_add_color_stop_rgba(pat, 1.0, cr_r, cr_g, cr_b, 0.0);
      cairo_set_source(cr, pat);
      cairo_arc(cr, nx, ny, gr, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
      cairo_pattern_destroy(pat);
    }

    if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT) {
      gdouble s = rad * 1.9;
      draw_rrect(cr, nx - s * 0.5, ny - s * 0.5, s, s, 3.0);
      set_tok(cr, BT_COL_SURFACE_2, alpha);
      cairo_fill_preserve(cr);
      set_tok(cr, is_hov ? BT_COL_ACCENT : col, alpha);
      cairo_set_line_width(cr, (is_hov ? 1.8 : 1.3) / zoom);
      cairo_stroke(cr);
      set_tok(cr, col, alpha);
      cairo_arc(cr, nx, ny, 2.2, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    } else if (nd->kind == BT_NODE_GHOST) {
      set_tok(cr, col, (nd->broken ? 0.35 : 0.80) * alpha);
      cairo_set_dash(cr, dashes, 2, 0.0);
      cairo_set_line_width(cr, 1.3 / zoom);
      cairo_arc(cr, nx, ny, rad, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
    } else {
      /* Subtle surface halo keeps edges cleanly separated from node fill. */
      if (!heavy) {
        set_tok(cr, BT_COL_SURFACE_1, alpha);
        cairo_arc(cr, nx, ny, rad + 1.5 / zoom, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
      }
      set_tok(cr, col, alpha);
      cairo_arc(cr, nx, ny, rad, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    }

    if (is_hov) {
      set_tok(cr, BT_COL_ACCENT, 0.4 + 0.6 * hover_fade);
      cairo_set_line_width(cr, 1.8 / zoom);
      cairo_arc(cr, nx, ny, rad + 3.5 / zoom, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
    }
  }

  /* Pass 3: HiDPI Pango labels for hovered/connected nodes and largest hubs. */
  if (!heavy) {
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Inter, Segoe UI, Sans 10");
    pango_font_description_set_size(fd, (gint)(10.5 * PANGO_SCALE / CLAMP(zoom, 0.65, 1.8)));
    pango_layout_set_font_description(pl, fd);

    guint deg_thresh = (g->nodes->len <= 24) ? 0u : 3u;
    for (guint i = 0; i < g->nodes->len; i++) {
      const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      if (!nd->visible || nd->label == NULL || nd->label[0] == '\0')
        continue;
      gboolean is_hov = ((gint)i == hover_idx);
      gboolean is_nbr = (adj != NULL && adj[i]);
      gboolean show_base = (nd->degree >= deg_thresh && zoom >= 0.55) ||
                           nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE;
      if (!is_hov && !is_nbr && !show_base)
        continue;

      gdouble nx, ny;
      node_pos(nd, &nx, &ny);
      if (nx < min_wx || nx > max_wx || ny < min_wy || ny > max_wy)
        continue;

      gdouble la = is_hov ? (0.45 + 0.55 * hover_fade)
                          : (is_nbr ? (0.35 + 0.55 * hover_fade) : (0.72 * dim_factor));
      if (la < 0.08)
        continue;

      pango_layout_set_text(pl, nd->label, -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      gdouble lx = nx - (gdouble)tw * 0.5;
      gdouble ly = ny + nd->radius + 5.0 / zoom;

      if (is_hov) {
        draw_rrect(cr, lx - 5.0 / zoom, ly - 2.0 / zoom,
                   (gdouble)tw + 10.0 / zoom, (gdouble)th + 4.0 / zoom, 4.0 / zoom);
        set_tok(cr, BT_COL_SURFACE_2, 0.92 * hover_fade);
        cairo_fill_preserve(cr);
        set_tok(cr, BT_COL_BORDER_STRONG, 0.9 * hover_fade);
        cairo_set_line_width(cr, 1.0 / zoom);
        cairo_stroke(cr);
      }
      set_tok(cr, is_hov ? BT_COL_TEXT_PRIMARY : BT_COL_TEXT_SECONDARY, la);
      cairo_move_to(cr, lx, ly);
      pango_cairo_show_layout(cr, pl);
    }

    if (has_stem) {
      pango_layout_set_text(pl, "sessions", -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      set_tok(cr, BT_COL_TEXT_MUTED, 0.75 * dim_factor);
      cairo_move_to(cr, (stem_min_x + stem_max_x) * 0.5 - (gdouble)tw * 0.5, 224.0);
      pango_cairo_show_layout(cr, pl);
    }

    pango_font_description_free(fd);
    g_object_unref(pl);
  }

  cairo_restore(cr);
}

void bt_graph_draw_cairo(const BtGraph *g, cairo_t *cr, int width, int height, gdouble pan_x,
                         gdouble pan_y, gdouble zoom, gint hover_idx, gboolean dark_mode)
{
  (void)dark_mode;
  draw_graph_internal(g, cr, width, height, pan_x, pan_y, zoom, hover_idx,
                      hover_idx >= 0 ? 1.0 : 0.0);
}

/* ------------------------------------------- camera fit & tick loop -- */

static void compute_fit_camera(BtGraphView *gv, int w, int h, gdouble *out_px, gdouble *out_py,
                               gdouble *out_zoom)
{
  if (gv->graph == NULL || gv->graph->nodes->len == 0 || w <= 20 || h <= 20) {
    *out_px = 0.0; *out_py = 0.0; *out_zoom = 1.0;
    return;
  }
  gdouble min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
  guint vis = 0;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (!nd->visible)
      continue;
    if (nd->x - nd->radius < min_x) min_x = nd->x - nd->radius;
    if (nd->x + nd->radius > max_x) max_x = nd->x + nd->radius;
    if (nd->y - nd->radius < min_y) min_y = nd->y - nd->radius;
    if (nd->y + nd->radius > max_y) max_y = nd->y + nd->radius;
    vis++;
  }
  if (vis == 0) {
    *out_px = 0.0; *out_py = 0.0; *out_zoom = 1.0;
    return;
  }
  gdouble bw = MAX(max_x - min_x, 120.0);
  gdouble bh = MAX(max_y - min_y, 120.0);
  gdouble zx = ((gdouble)w - 96.0) / bw;
  gdouble zy = ((gdouble)h - 96.0) / bh;
  gdouble z = CLAMP(MIN(zx, zy), 0.35, 1.6);
  gdouble cx = (min_x + max_x) * 0.5;
  gdouble cy = (min_y + max_y) * 0.5;
  *out_zoom = z;
  *out_px = -cx * z;
  *out_py = -cy * z;
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

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer ud)
{
  (void)clock;
  BtGraphView *gv = ud;
  if (gv->area == NULL || !gtk_widget_get_mapped(widget)) {
    gv->tick_id = 0;
    return G_SOURCE_REMOVE;
  }

  gboolean active = FALSE;

  /* 1. Node growth animation. */
  if (gv->graph != NULL) {
    for (guint i = 0; i < gv->graph->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      if (nd->birth_t < 1.0) {
        nd->birth_t = MIN(nd->birth_t + 0.09, 1.0);
        active = TRUE;
      }
    }
  }

  /* 2. Hover fade animation. */
  gdouble target_hf = (gv->hover_idx >= 0) ? 1.0 : 0.0;
  if (fabs(gv->hover_fade - target_hf) > 0.02) {
    gv->hover_fade += (target_hf - gv->hover_fade) * 0.25;
    active = TRUE;
  } else {
    gv->hover_fade = target_hf;
  }

  /* 3. Smooth camera transition (zoom / fit / reset buttons). */
  if (gv->animating_cam) {
    gv->pan_x += (gv->target_pan_x - gv->pan_x) * 0.25;
    gv->pan_y += (gv->target_pan_y - gv->pan_y) * 0.25;
    gv->zoom += (gv->target_zoom - gv->zoom) * 0.25;
    if (fabs(gv->pan_x - gv->target_pan_x) < 0.5 &&
        fabs(gv->pan_y - gv->target_pan_y) < 0.5 &&
        fabs(gv->zoom - gv->target_zoom) < 0.005) {
      gv->pan_x = gv->target_pan_x;
      gv->pan_y = gv->target_pan_y;
      gv->zoom = gv->target_zoom;
      gv->animating_cam = FALSE;
    } else {
      active = TRUE;
    }
  }

  gtk_widget_queue_draw(gv->area);
  if (!active) {
    gv->tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void ensure_tick(BtGraphView *gv)
{
  if (gv->area == NULL || gv->tick_id != 0 || !gtk_widget_get_mapped(gv->area))
    return;
  gv->tick_id = gtk_widget_add_tick_callback(gv->area, on_tick, gv, NULL);
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
  (void)area;
  BtGraphView *gv = ud;
  if (gv->needs_fit && gv->graph != NULL && gv->graph->nodes->len > 0 && w > 20 && h > 20) {
    compute_fit_camera(gv, w, h, &gv->pan_x, &gv->pan_y, &gv->zoom);
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
    gv->target_zoom = gv->zoom;
    gv->animating_cam = FALSE;
    gv->needs_fit = FALSE;
  }
  draw_graph_internal(gv->graph, cr, w, h, gv->pan_x, gv->pan_y, gv->zoom,
                      gv->hover_idx, gv->hover_fade);
}

static void screen_to_world(BtGraphView *gv, gdouble sx, gdouble sy, gdouble *wx, gdouble *wy)
{
  int w = gtk_widget_get_width(gv->area);
  int h = gtk_widget_get_height(gv->area);
  *wx = (sx - w * 0.5 - gv->pan_x) / gv->zoom;
  *wy = (sy - h * 0.5 - gv->pan_y) / gv->zoom;
}

/* ---------------------------------------------- pointer & overlay UI -- */

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
  gv->animating_cam = FALSE;
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
      ensure_tick(gv);
    }
  }
}

static void on_motion_leave(GtkEventControllerMotion *c, gpointer ud)
{
  (void)c;
  BtGraphView *gv = ud;
  if (gv->hover_idx != -1) {
    gv->hover_idx = -1;
    ensure_tick(gv);
  }
}

static gboolean on_scroll(GtkEventControllerScroll *c, gdouble dx, gdouble dy, gpointer ud)
{
  (void)c; (void)dx;
  BtGraphView *gv = ud;
  gv->animating_cam = FALSE;
  gv->zoom = CLAMP(gv->zoom * (dy < 0 ? 1.12 : 0.89), 0.2, 4.0);
  gtk_widget_queue_draw(gv->area);
  return TRUE;
}

static void on_btn_zoom_in(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->target_pan_x = gv->pan_x;
  gv->target_pan_y = gv->pan_y;
  gv->target_zoom = CLAMP(gv->zoom * 1.25, 0.2, 4.0);
  gv->animating_cam = TRUE;
  ensure_tick(gv);
}

static void on_btn_zoom_out(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->target_pan_x = gv->pan_x;
  gv->target_pan_y = gv->pan_y;
  gv->target_zoom = CLAMP(gv->zoom * 0.8, 0.2, 4.0);
  gv->animating_cam = TRUE;
  ensure_tick(gv);
}

static void on_btn_fit(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (gv->area == NULL)
    return;
  compute_fit_camera(gv, gtk_widget_get_width(gv->area), gtk_widget_get_height(gv->area),
                     &gv->target_pan_x, &gv->target_pan_y, &gv->target_zoom);
  gv->animating_cam = TRUE;
  ensure_tick(gv);
}

static void on_btn_reset(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->target_pan_x = 0.0;
  gv->target_pan_y = 0.0;
  gv->target_zoom = 1.0;
  gv->animating_cam = TRUE;
  ensure_tick(gv);
}

static void on_area_map(GtkWidget *area, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->graph != NULL && graph_has_pending_birth(gv->graph))
    ensure_tick(gv);
  gtk_widget_queue_draw(area);
}

static void on_area_destroy(GtkWidget *area, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->tick_id != 0) {
    gtk_widget_remove_tick_callback(area, gv->tick_id);
    gv->tick_id = 0;
  }
  gv->area = NULL;
  gv->overlay = NULL;
  gv->root = NULL;
}

static GtkWidget *make_ctrl_btn(const gchar *icon, const gchar *tip, GCallback cb, gpointer ud)
{
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(b, "bt-iconbtn");
  gtk_widget_set_tooltip_text(b, tip);
  g_signal_connect(b, "clicked", cb, ud);
  return b;
}

/* ---------------------------------------------------------- public API -- */

BtGraphView *bt_graphview_new(BtGraphOpenNoteFn on_open, BtGraphOpenTermFn on_open_term,
                              gpointer user_data)
{
  BtGraphView *gv = g_new0(BtGraphView, 1);
  gv->zoom = 1.0;
  gv->target_zoom = 1.0;
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
  g_signal_connect(mot, "leave", G_CALLBACK(on_motion_leave), gv);
  gtk_widget_add_controller(gv->area, mot);

  GtkEventController *scr = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scr, "scroll", G_CALLBACK(on_scroll), gv);
  gtk_widget_add_controller(gv->area, scr);

  /* Overlay controls (top-right) & mini legend (bottom-left). */
  gv->overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(gv->overlay, TRUE);
  gtk_widget_set_vexpand(gv->overlay, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(gv->overlay), gv->area);

  GtkWidget *ctrls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(ctrls, "bt-graph-controls");
  gtk_widget_set_halign(ctrls, GTK_ALIGN_END);
  gtk_widget_set_valign(ctrls, GTK_ALIGN_START);
  gtk_widget_set_margin_top(ctrls, 10);
  gtk_widget_set_margin_end(ctrls, 10);
  gtk_box_append(GTK_BOX(ctrls), make_ctrl_btn("bt-zoom-in-symbolic", "Zoom in",
                                               G_CALLBACK(on_btn_zoom_in), gv));
  gtk_box_append(GTK_BOX(ctrls), make_ctrl_btn("bt-zoom-out-symbolic", "Zoom out",
                                               G_CALLBACK(on_btn_zoom_out), gv));
  gtk_box_append(GTK_BOX(ctrls), make_ctrl_btn("bt-fit-symbolic", "Fit graph",
                                               G_CALLBACK(on_btn_fit), gv));
  gtk_box_append(GTK_BOX(ctrls), make_ctrl_btn("bt-reset-symbolic", "Reset view",
                                               G_CALLBACK(on_btn_reset), gv));
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), ctrls);

  GtkWidget *legend = gtk_label_new(NULL);
  gtk_label_set_markup(
      GTK_LABEL(legend),
      "<span foreground='#E6E8EC'>●</span> Note  "
      "<span foreground='#8B7CF6'>●</span> AI  "
      "<span foreground='#FBBF24'>●</span> Tag  "
      "<span foreground='#4ADE80'>●</span> Folder");
  gtk_widget_add_css_class(legend, "bt-graph-legend");
  gtk_widget_set_halign(legend, GTK_ALIGN_START);
  gtk_widget_set_valign(legend, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(legend, 10);
  gtk_widget_set_margin_start(legend, 10);
  gtk_widget_set_can_target(legend, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), legend);

  /* Friendly empty state when graph has 0 nodes. */
  GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(empty, "bt-empty");
  gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
  GtkWidget *eico = gtk_image_new_from_icon_name("bt-graph-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(eico), 36);
  GtkWidget *etitle = gtk_label_new("Graph is empty");
  gtk_widget_add_css_class(etitle, "bt-empty-title");
  GtkWidget *esub = gtk_label_new("Create or link notes with [[wiki-links]] to grow the graph");
  gtk_box_append(GTK_BOX(empty), eico);
  gtk_box_append(GTK_BOX(empty), etitle);
  gtk_box_append(GTK_BOX(empty), esub);

  gv->root = gtk_stack_new();
  gtk_widget_set_hexpand(gv->root, TRUE);
  gtk_widget_set_vexpand(gv->root, TRUE);
  gtk_stack_add_named(GTK_STACK(gv->root), gv->overlay, "canvas");
  gtk_stack_add_named(GTK_STACK(gv->root), empty, "empty");
  gtk_stack_set_visible_child_name(GTK_STACK(gv->root), "empty");

  return gv;
}

void bt_graphview_free(BtGraphView *gv)
{
  if (gv == NULL)
    return;
  if (gv->tick_id != 0 && gv->area != NULL) {
    gtk_widget_remove_tick_callback(gv->area, gv->tick_id);
    gv->tick_id = 0;
  }
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
  gv->hover_fade = 0.0;
  gv->needs_fit = TRUE;

  gboolean has_nodes = (gv->graph != NULL && gv->graph->nodes->len > 0);
  if (gv->root != NULL)
    gtk_stack_set_visible_child_name(GTK_STACK(gv->root), has_nodes ? "canvas" : "empty");

  if (has_nodes && animate_growth) {
    for (guint i = 0; i < gv->graph->nodes->len; i++)
      ((BtGraphNode *)g_ptr_array_index(gv->graph->nodes, i))->birth_t = 0.05;
    if (gv->area != NULL && gtk_widget_get_mapped(gv->area))
      ensure_tick(gv);
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
