/* graphview.c — Brain Graph v2 widget & upgraded Cairo 2D fallback renderer:
 *   - Unified BtGraphRenderer vtable (ready for Step 2 GtkGLArea backend).
 *   - Atmospheric dark canvas (@bt_graph_bg) + pre-baked radial vignette (@bt_graph_vignette).
 *   - Pre-baked radial-gradient glow sprites per node kind (zero per-frame pattern alloc).
 *   - Thin low-alpha edges: additive curved arcs on High (<= 600 edges), batched straight
 *     strokes under heavy/Low load for sustained 60 FPS.
 *   - Sparse drifting dust particles (pre-allocated fixed table; stopped by Reduce Motion /
 *     unfocused window / Low idle).
 *   - Smooth Pango label fade driven by bt_graph_label_alpha().
 *   - Toolbar toggles for Quality (Low / High) and Reduce Motion, plus zoom/fit/reset. */
#include "graphview.h"
#include "graph_state.h"
#include "theme.h"
#include "ui/graphgl.h"
#include "ui/graphview_renderer.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BT_GLOW_SPRITE_SZ 32
#define BT_VIGNETTE_SZ    128
#define BT_DUST_MAX       64
#define BT_SPRITE_KINDS   7 /* 6 BtNodeKind + 1 accent hover glow */
#define BT_SPRITE_ACCENT  6

typedef struct {
  gdouble x0, y0;
  gdouble vx, vy;
  gdouble radius;
  gdouble alpha;
} BtDustParticle;

typedef struct {
  cairo_surface_t *glow[BT_SPRITE_KINDS];
  cairo_surface_t *vignette;
  BtDustParticle dust[BT_DUST_MAX];
  gboolean initialized;
} BtCairoPriv;

/* ----------------------------------------------------- color & sprites -- */

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

static guint sprite_slot_for_kind(BtNodeKind k)
{
  guint idx = (guint)k;
  return idx < 6u ? idx : 5u;
}

static cairo_surface_t *bake_glow_sprite(guint32 rgb)
{
  cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BT_GLOW_SPRITE_SZ, BT_GLOW_SPRITE_SZ);
  cairo_t *cr = cairo_create(surf);
  gdouble c = (gdouble)BT_GLOW_SPRITE_SZ * 0.5;
  gdouble r, g, b;
  bt_theme_unpack(rgb, &r, &g, &b);

  cairo_pattern_t *pat = cairo_pattern_create_radial(c, c, c * 0.06, c, c, c);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, 0.92);
  cairo_pattern_add_color_stop_rgba(pat, 0.28, r, g, b, 0.42);
  cairo_pattern_add_color_stop_rgba(pat, 0.65, r, g, b, 0.10);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, 0.0);
  cairo_set_source(cr, pat);
  cairo_rectangle(cr, 0, 0, BT_GLOW_SPRITE_SZ, BT_GLOW_SPRITE_SZ);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);
  cairo_destroy(cr);
  return surf;
}

static cairo_surface_t *bake_vignette_surface(void)
{
  cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BT_VIGNETTE_SZ, BT_VIGNETTE_SZ);
  cairo_t *cr = cairo_create(surf);
  gdouble c = (gdouble)BT_VIGNETTE_SZ * 0.5;
  gdouble r, g, b;
  bt_theme_unpack(BT_COL_GRAPH_VIGNETTE, &r, &g, &b);

  cairo_pattern_t *pat = cairo_pattern_create_radial(c, c, c * 0.25, c, c, c * 1.38);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, 0.0);
  cairo_pattern_add_color_stop_rgba(pat, 0.65, r, g, b, 0.22);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, 0.78);
  cairo_set_source(cr, pat);
  cairo_rectangle(cr, 0, 0, BT_VIGNETTE_SZ, BT_VIGNETTE_SZ);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);
  cairo_destroy(cr);
  return surf;
}

static guint32 lcg_next(guint32 *s)
{
  *s = (*s) * 1664525u + 1013904223u;
  return *s;
}

static gboolean cairo_backend_init(BtGraphRenderer *r)
{
  BtCairoPriv *p = r->priv;
  if (p->initialized)
    return TRUE;

  static const BtNodeKind kinds[6] = {
    BT_NODE_HUMAN_NOTE, BT_NODE_AI_NOTE, BT_NODE_TAG,
    BT_NODE_SESSION, BT_NODE_IMPORT_ROOT, BT_NODE_GHOST,
  };
  for (guint i = 0; i < 6; i++)
    p->glow[i] = bake_glow_sprite(node_color(kinds[i]));
  p->glow[BT_SPRITE_ACCENT] = bake_glow_sprite(BT_COL_GRAPH_NODE_GLOW);
  p->vignette = bake_vignette_surface();

  guint32 seed = 0xB7A1E29Du;
  for (guint i = 0; i < BT_DUST_MAX; i++) {
    gdouble u0 = (gdouble)(lcg_next(&seed) & 0xFFFFu) / 65535.0;
    gdouble u1 = (gdouble)(lcg_next(&seed) & 0xFFFFu) / 65535.0;
    gdouble u2 = (gdouble)(lcg_next(&seed) & 0xFFFFu) / 65535.0;
    gdouble u3 = (gdouble)(lcg_next(&seed) & 0xFFFFu) / 65535.0;
    gdouble u4 = (gdouble)(lcg_next(&seed) & 0xFFFFu) / 65535.0;
    p->dust[i].x0 = u0;
    p->dust[i].y0 = u1;
    p->dust[i].vx = (u2 - 0.5) * 0.011;
    p->dust[i].vy = -0.004 - u3 * 0.008;
    p->dust[i].radius = 0.8 + u4 * 1.4;
    p->dust[i].alpha = 0.08 + u4 * 0.16;
  }

  p->initialized = TRUE;
  return TRUE;
}

static void cairo_backend_destroy(BtGraphRenderer *r)
{
  if (r == NULL)
    return;
  BtCairoPriv *p = r->priv;
  if (p != NULL) {
    for (guint i = 0; i < BT_SPRITE_KINDS; i++)
      g_clear_pointer(&p->glow[i], cairo_surface_destroy);
    g_clear_pointer(&p->vignette, cairo_surface_destroy);
    g_free(p);
  }
  g_free(r);
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
    gdouble t = 1.0 - pow(1.0 - nd->birth_t, 3.0);
    *x = nd->x * t;
    *y = nd->y * t;
  }
}

static inline void stamp_glow(cairo_t *cr, cairo_surface_t *sprite, gdouble x, gdouble y,
                              gdouble radius, gdouble alpha, gboolean fast_filter)
{
  if (sprite == NULL || alpha <= 0.01 || radius <= 0.5)
    return;
  gdouble scale = (radius * 2.0) / (gdouble)BT_GLOW_SPRITE_SZ;
  cairo_save(cr);
  cairo_translate(cr, x - radius, y - radius);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, sprite, 0.0, 0.0);
  if (fast_filter)
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
}

static void draw_dust(cairo_t *cr, const BtCairoPriv *p, const BtGraphRenderState *st)
{
  guint count = (st->quality == BT_GRAPH_QUALITY_HIGH) ? BT_DUST_MAX : (BT_DUST_MAX / 3u);
  gdouble t = st->reduce_motion ? 0.0 : st->time_sec;
  gdouble w = (gdouble)st->width;
  gdouble h = (gdouble)st->height;

  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
  if (st->quality == BT_GRAPH_QUALITY_HIGH)
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  set_tok(cr, BT_COL_GRAPH_DUST, st->quality == BT_GRAPH_QUALITY_HIGH ? 0.20 : 0.12);

  for (guint i = 0; i < count; i++) {
    const BtDustParticle *dp = &p->dust[i];
    gdouble u = fmod(dp->x0 + dp->vx * t, 1.0);
    gdouble v = fmod(dp->y0 + dp->vy * t, 1.0);
    if (u < 0.0) u += 1.0;
    if (v < 0.0) v += 1.0;
    gdouble sx = u * w;
    gdouble sy = v * h;
    gdouble rr = dp->radius;
    cairo_rectangle(cr, sx - rr * 0.5, sy - rr * 0.5, rr, rr);
  }
  cairo_fill(cr);
  cairo_restore(cr);
}

static void cairo_backend_render(BtGraphRenderer *r, cairo_t *cr, const BtGraphRenderState *st)
{
  BtCairoPriv *p = r->priv;
  if (!p->initialized)
    cairo_backend_init(r);

  int width = st->width;
  int height = st->height;
  if (width <= 0 || height <= 0)
    return;

  /* 1. Deep atmospheric canvas + pre-baked radial vignette (zero per-frame alloc). */
  set_tok(cr, BT_COL_GRAPH_BG, 1.0);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (p->vignette != NULL) {
    cairo_save(cr);
    cairo_scale(cr, (gdouble)width / (gdouble)BT_VIGNETTE_SZ,
                (gdouble)height / (gdouble)BT_VIGNETTE_SZ);
    cairo_set_source_surface(cr, p->vignette, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
  }

  const BtGraph *g = st->graph;
  if (g == NULL || g->nodes->len == 0)
    return;

  gboolean high = (st->quality == BT_GRAPH_QUALITY_HIGH);
  gboolean heavy = (g->nodes->len > 400u);
  gboolean very_heavy = (g->nodes->len > 1500u);

  /* 2. Sparse drifting atmospheric dust particles. */
  if (!very_heavy || high)
    draw_dust(cr, p, st);

  cairo_save(cr);
  cairo_set_antialias(cr, heavy ? CAIRO_ANTIALIAS_FAST : CAIRO_ANTIALIAS_GOOD);
  cairo_translate(cr, width * 0.5 + st->pan_x, height * 0.5 + st->pan_y);
  cairo_scale(cr, st->zoom, st->zoom);

  gdouble zoom = st->zoom;
  gdouble min_wx = (-width * 0.5 - st->pan_x) / zoom - 40.0;
  gdouble max_wx = (width * 0.5 - st->pan_x) / zoom + 40.0;
  gdouble min_wy = (-height * 0.5 - st->pan_y) / zoom - 40.0;
  gdouble max_wy = (height * 0.5 - st->pan_y) / zoom + 40.0;

  /* Subtle session dock divider when session/import nodes exist on small/medium vaults. */
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
      set_tok(cr, BT_COL_BORDER_STRONG, 0.45);
      cairo_set_line_width(cr, 1.0 / zoom);
      cairo_move_to(cr, stem_min_x - pad, 238.0);
      cairo_line_to(cr, stem_max_x + pad, 238.0);
      cairo_stroke(cr);
    }
  }

  gint focus_idx = (st->hover_idx >= 0) ? st->hover_idx : st->focus_idx;
  gboolean has_focus = (focus_idx >= 0 && (guint)focus_idx < g->nodes->len);
  gdouble hf = has_focus ? CLAMP(st->hover_fade, 0.0, 1.0) : 0.0;
  if (st->hover_idx < 0 && st->focus_idx >= 0)
    hf = 1.0;
  gdouble dim_factor = has_focus ? (1.0 - 0.75 * hf) : 1.0;
  gboolean use_curved_edges = (high && g->edges->len <= 600u);
  gboolean lod_collapse = (g->nodes->len > 2000u && zoom < 0.6);

  /* Pass 1: base edges — additive glow on High, fast batched stroke on heavy/Low. */
  cairo_save(cr);
  cairo_set_antialias(cr, (heavy || !high) ? CAIRO_ANTIALIAS_NONE : CAIRO_ANTIALIAS_GOOD);
  if (high && !very_heavy)
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, (high ? 1.0 : 0.9) / zoom);
  set_tok(cr, BT_COL_GRAPH_EDGE_DIM, (high ? 0.22 : 0.16) * dim_factor);

  for (guint i = 0; i < g->edges->len; i++) {
    const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
    if (lod_collapse && e->kind == BT_EDGE_TAGGED)
      continue;
    if (has_focus && ((gint)e->src_idx == focus_idx || (gint)e->dst_idx == focus_idx))
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
    if (use_curved_edges) {
      gdouble mx = (ax + bx) * 0.5;
      gdouble my = (ay + by) * 0.5;
      gdouble dx = bx - ax;
      gdouble dy = by - ay;
      gdouble sign = ((e->src_idx ^ e->dst_idx) & 1u) ? 0.11 : -0.11;
      gdouble cx = mx - dy * sign;
      gdouble cy = my + dx * sign;
      cairo_curve_to(cr, (ax + 2.0 * cx) / 3.0, (ay + 2.0 * cy) / 3.0,
                     (bx + 2.0 * cx) / 3.0, (by + 2.0 * cy) / 3.0, bx, by);
    } else {
      cairo_line_to(cr, bx, by);
    }
  }
  cairo_stroke(cr);
  cairo_restore(cr);

  /* Pass 1b: highlighted incident edges on hover/focus (additive luminous accent). */
  if (has_focus) {
    cairo_save(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    if (high)
      cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_set_line_width(cr, 1.7 / zoom);
    set_tok(cr, BT_COL_GRAPH_EDGE_ACTIVE, 0.30 + 0.62 * hf);
    for (guint i = 0; i < g->edges->len; i++) {
      const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
      if ((gint)e->src_idx != focus_idx && (gint)e->dst_idx != focus_idx)
        continue;
      const BtGraphNode *a = g_ptr_array_index(g->nodes, e->src_idx);
      const BtGraphNode *b = g_ptr_array_index(g->nodes, e->dst_idx);
      if (!a->visible || !b->visible)
        continue;
      gdouble ax, ay, bx, by;
      node_pos(a, &ax, &ay);
      node_pos(b, &bx, &by);
      cairo_move_to(cr, ax, ay);
      if (use_curved_edges) {
        gdouble mx = (ax + bx) * 0.5;
        gdouble my = (ay + by) * 0.5;
        gdouble dx = bx - ax;
        gdouble dy = by - ay;
        gdouble sign = ((e->src_idx ^ e->dst_idx) & 1u) ? 0.11 : -0.11;
        gdouble cx = mx - dy * sign;
        gdouble cy = my + dx * sign;
        cairo_curve_to(cr, (ax + 2.0 * cx) / 3.0, (ay + 2.0 * cy) / 3.0,
                       (bx + 2.0 * cx) / 3.0, (by + 2.0 * cy) / 3.0, bx, by);
      } else {
        cairo_line_to(cr, bx, by);
      }
    }
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  /* Pass 2a: pre-baked radial glow halos (all nodes <= 400; hubs/AI/focused on heavy). */
  gboolean draw_all_halos = high && !heavy;
  gboolean draw_hub_halos = high && heavy && !very_heavy;
  if (draw_all_halos || draw_hub_halos || has_focus || st->selected_idx >= 0) {
    cairo_save(cr);
    if (high)
      cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    for (guint i = 0; i < g->nodes->len; i++) {
      const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      if (!nd->visible || nd->kind == BT_NODE_GHOST)
        continue;
      guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
      gboolean is_foc = ((gint)i == focus_idx);
      gboolean is_sel = ((gint)i == st->selected_idx);
      gboolean is_luminous_hub =
          draw_hub_halos && (nd->kind == BT_NODE_AI_NOTE || nd->degree >= st->hub_thresh);
      if (!draw_all_halos && !is_luminous_hub && !is_foc && !is_sel && nbr != BT_NBR_NEIGHBOR)
        continue;

      gdouble nx, ny;
      node_pos(nd, &nx, &ny);
      if (nx < min_wx || nx > max_wx || ny < min_wy || ny > max_wy)
        continue;

      gdouble alpha = (has_focus && nbr == BT_NBR_NONE && !is_sel) ? dim_factor : 1.0;
      gdouble halo_r = nd->radius * (is_foc ? (3.2 + 0.9 * hf) : (is_sel ? 3.0 : 2.5));
      gdouble halo_a = is_foc ? (0.45 + 0.45 * hf)
                              : ((nbr == BT_NBR_NEIGHBOR || is_sel) ? 0.55 : (0.34 * alpha));
      cairo_surface_t *sp =
          (is_foc || is_sel) ? p->glow[BT_SPRITE_ACCENT] : p->glow[sprite_slot_for_kind(nd->kind)];
      stamp_glow(cr, sp, nx, ny, halo_r, halo_a, heavy);
    }
    cairo_restore(cr);
  }

  /* Pass 2b: crisp luminous node cores. */
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

    guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
    gboolean is_foc = ((gint)i == focus_idx);
    gboolean is_sel = ((gint)i == st->selected_idx);
    gdouble alpha = (has_focus && nbr == BT_NBR_NONE && !is_sel) ? dim_factor : 1.0;
    guint32 col = node_color(nd->kind);
    gdouble rad = nd->radius;

    if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT) {
      gdouble s = rad * 1.9;
      draw_rrect(cr, nx - s * 0.5, ny - s * 0.5, s, s, 3.0);
      set_tok(cr, BT_COL_SURFACE_2, alpha);
      cairo_fill_preserve(cr);
      set_tok(cr, (is_foc || is_sel) ? BT_COL_GRAPH_NODE_GLOW : col, alpha);
      cairo_set_line_width(cr, ((is_foc || is_sel) ? 1.8 : 1.3) / zoom);
      cairo_stroke(cr);
      set_tok(cr, col, alpha);
      cairo_arc(cr, nx, ny, 2.2, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    } else if (nd->kind == BT_NODE_GHOST) {
      set_tok(cr, col, (nd->broken ? 0.30 : 0.78) * alpha);
      cairo_set_dash(cr, dashes, 2, 0.0);
      cairo_set_line_width(cr, 1.3 / zoom);
      cairo_arc(cr, nx, ny, rad, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
    } else {
      set_tok(cr, col, alpha);
      cairo_arc(cr, nx, ny, rad, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
      /* Pinpoint luminous inner core on High quality. */
      if (high && !very_heavy && rad >= 4.0) {
        set_tok(cr, BT_COL_TEXT_PRIMARY, 0.55 * alpha);
        cairo_arc(cr, nx, ny, rad * 0.42, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
      }
    }

    if (is_foc || is_sel) {
      set_tok(cr, BT_COL_GRAPH_NODE_GLOW, is_sel ? 0.95 : (0.45 + 0.55 * hf));
      cairo_set_line_width(cr, 1.8 / zoom);
      cairo_arc(cr, nx, ny, rad + 3.5 / zoom, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
    }
  }

  /* Pass 3: crisp fading Pango labels (driven by bt_graph_label_alpha()). */
  if (!very_heavy || has_focus || st->selected_idx >= 0) {
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Inter, Segoe UI, Sans 10");
    pango_font_description_set_size(fd, (gint)(10.5 * PANGO_SCALE / CLAMP(zoom, 0.65, 1.8)));
    pango_layout_set_font_description(pl, fd);

    guint drawn_labels = 0;
    const guint label_cap = heavy ? 28u : 120u;
    for (guint i = 0; i < g->nodes->len; i++) {
      const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
      gboolean is_sel = ((gint)i == st->selected_idx);
      gboolean is_kbd = ((gint)i == st->focus_idx);
      gboolean priority = (nbr == BT_NBR_FOCUS || nbr == BT_NBR_NEIGHBOR || is_sel || is_kbd);
      if (!priority && drawn_labels >= label_cap)
        continue;

      gdouble la = bt_graph_label_alpha(nd, nbr, is_sel, is_kbd, st->hub_thresh, zoom,
                                        has_focus, hf, g->nodes->len);
      if (la < 0.06)
        continue;

      gdouble nx, ny;
      node_pos(nd, &nx, &ny);
      if (nx < min_wx || nx > max_wx || ny < min_wy || ny > max_wy)
        continue;

      pango_layout_set_text(pl, nd->label, -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      gdouble lx = nx - (gdouble)tw * 0.5;
      gdouble ly = ny + nd->radius + 5.0 / zoom;

      if (nbr == BT_NBR_FOCUS || is_sel || is_kbd) {
        draw_rrect(cr, lx - 5.0 / zoom, ly - 2.0 / zoom,
                   (gdouble)tw + 10.0 / zoom, (gdouble)th + 4.0 / zoom, 4.0 / zoom);
        set_tok(cr, BT_COL_SURFACE_2, 0.92 * la);
        cairo_fill_preserve(cr);
        set_tok(cr, (is_sel || is_kbd) ? BT_COL_ACCENT : BT_COL_BORDER_STRONG, 0.9 * la);
        cairo_set_line_width(cr, 1.0 / zoom);
        cairo_stroke(cr);
      }
      set_tok(cr, (nbr == BT_NBR_FOCUS || is_sel || is_kbd) ? BT_COL_TEXT_PRIMARY
                                                            : BT_COL_TEXT_SECONDARY,
              la);
      cairo_move_to(cr, lx, ly);
      pango_cairo_show_layout(cr, pl);
      drawn_labels++;
    }

    if (has_stem) {
      pango_layout_set_text(pl, "sessions", -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      set_tok(cr, BT_COL_TEXT_MUTED, 0.72 * dim_factor);
      cairo_move_to(cr, (stem_min_x + stem_max_x) * 0.5 - (gdouble)tw * 0.5, 224.0);
      pango_cairo_show_layout(cr, pl);
    }

    pango_font_description_free(fd);
    g_object_unref(pl);
  }

  cairo_restore(cr);
}

static const BtGraphRendererVTable CAIRO_VT = {
  .name = "cairo-2d",
  .backend = BT_GRAPH_BACKEND_CAIRO,
  .init = cairo_backend_init,
  .upload_graph = NULL,
  .upload_layout3d = NULL,
  .resize = NULL,
  .render_cairo = cairo_backend_render,
  .render_gl = NULL,
  .destroy = cairo_backend_destroy,
};

BtGraphRenderer *bt_graph_cairo_renderer_new(void)
{
  BtGraphRenderer *r = g_new0(BtGraphRenderer, 1);
  r->vt = &CAIRO_VT;
  r->priv = g_new0(BtCairoPriv, 1);
  cairo_backend_init(r);
  return r;
}

/* Headless convenience entry point (reuses a static renderer cache so repeated
 * bench/test calls incur zero sprite allocation overhead). */
void bt_graph_draw_cairo(const BtGraph *g, cairo_t *cr, int width, int height, gdouble pan_x,
                         gdouble pan_y, gdouble zoom, gint hover_idx, gboolean dark_mode)
{
  (void)dark_mode;
  static BtGraphRenderer *shared_r = NULL;
  if (shared_r == NULL)
    shared_r = bt_graph_cairo_renderer_new();

  guint n = (g != NULL && g->nodes != NULL) ? g->nodes->len : 0u;
  g_autofree guint8 *mask = (hover_idx >= 0 && n > 0u) ? g_new0(guint8, n) : NULL;
  if (mask != NULL)
    bt_graph_compute_neighbors(g, hover_idx, mask, n);

  BtGraphRenderState st = {
    .graph = g,
    .width = width,
    .height = height,
    .scale = 1,
    .pan_x = pan_x,
    .pan_y = pan_y,
    .zoom = zoom,
    .hover_idx = hover_idx,
    .selected_idx = -1,
    .focus_idx = -1,
    .hover_fade = hover_idx >= 0 ? 1.0 : 0.0,
    .nbr_mask = mask,
    .nbr_mask_len = n,
    .hub_thresh = bt_graph_hub_degree_threshold(g),
    .time_sec = 0.0,
    .quality = (n > 400u) ? BT_GRAPH_QUALITY_LOW : BT_GRAPH_QUALITY_HIGH,
    .mode = BT_GRAPH_MODE_2D,
    .reduce_motion = TRUE,
  };
  shared_r->vt->render_cairo(shared_r, cr, &st);
}

/* ------------------------------------------- BtGraphView GTK4 widget -- */

struct BtGraphView {
  GtkWidget *root;         /* GtkStack: "canvas" | "empty" */
  GtkWidget *overlay;      /* GtkOverlay wrapping canvas_stack + controls + legend */
  GtkWidget *canvas_stack; /* GtkStack ("bt-graph-area"): "gl" | "cairo" */
  GtkWidget *cairo_area;   /* GtkDrawingArea fallback */
  BtGraphGL *gl;           /* GtkGLArea backend */
  GtkWidget *btn_quality;
  GtkWidget *btn_motion;
  GtkWindow *toplevel;
  gulong toplevel_active_sig;

  BtGraph *graph;
  BtGraphRenderer *cairo_renderer;
  BtGraphBackend active_backend;
  gchar *pending_notice;
  guint8 *nbr_mask;
  guint nbr_mask_cap;
  guint hub_thresh;

  gdouble pan_x, pan_y, zoom;
  gdouble target_pan_x, target_pan_y, target_zoom;
  gboolean animating_cam;
  gboolean needs_fit;
  gboolean dark_mode;
  gboolean window_active;
  BtGraphQuality quality;
  gboolean reduce_motion;

  gboolean dragging;
  gdouble drag_start_x, drag_start_y;
  gdouble pan_start_x, pan_start_y;

  gint hover_idx;
  gint selected_idx;
  gint focus_idx;
  gdouble hover_fade;
  gdouble time_sec;
  gint64 last_tick_us;
  guint tick_id;

  BtGraphOpenNoteFn on_open;
  BtGraphOpenTermFn on_open_term;
  BtGraphNoticeFn on_notice;
  gpointer user_data;
  gpointer notice_ud;
};

static void queue_canvas_draw(BtGraphView *gv)
{
  if (gv == NULL)
    return;
  if (gv->active_backend == BT_GRAPH_BACKEND_GL && gv->gl != NULL && bt_graph_gl_is_ready(gv->gl))
    bt_graph_gl_queue_render(gv->gl);
  else if (gv->cairo_area != NULL)
    gtk_widget_queue_draw(gv->cairo_area);
}

static void emit_notice(BtGraphView *gv, const gchar *msg)
{
  g_free(gv->pending_notice);
  gv->pending_notice = g_strdup(msg);
  if (gv->on_notice != NULL && msg != NULL)
    gv->on_notice(msg, gv->notice_ud);
}

static void ensure_nbr_mask(BtGraphView *gv, guint n)
{
  if (n > gv->nbr_mask_cap) {
    g_free(gv->nbr_mask);
    gv->nbr_mask = g_new0(guint8, n);
    gv->nbr_mask_cap = n;
  }
}

static void refresh_neighbor_mask(BtGraphView *gv)
{
  if (gv->graph == NULL || gv->graph->nodes->len == 0)
    return;
  guint n = gv->graph->nodes->len;
  ensure_nbr_mask(gv, n);
  gint foc = (gv->hover_idx >= 0) ? gv->hover_idx : gv->focus_idx;
  bt_graph_compute_neighbors(gv->graph, foc, gv->nbr_mask, n);
  bt_graph_gl_mark_state_dirty(gv->gl);
}

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

static gboolean needs_ambient_tick(const BtGraphView *gv)
{
  if (gv->graph == NULL || gv->graph->nodes->len == 0)
    return FALSE;
  if (gv->quality != BT_GRAPH_QUALITY_HIGH || gv->reduce_motion || !gv->window_active)
    return FALSE;
  return gv->active_backend == BT_GRAPH_BACKEND_GL || gv->graph->nodes->len <= 1500u;
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer ud)
{
  (void)clock;
  BtGraphView *gv = ud;
  if (gv->canvas_stack == NULL || !gtk_widget_get_mapped(widget)) {
    gv->tick_id = 0;
    gv->last_tick_us = 0;
    return G_SOURCE_REMOVE;
  }

  gint64 now = g_get_monotonic_time();
  gdouble dt = (gv->last_tick_us > 0) ? CLAMP((gdouble)(now - gv->last_tick_us) / 1e6, 0.0, 0.05)
                                      : 0.016;
  gv->last_tick_us = now;

  gboolean active = FALSE;

  /* 1. Node growth animation (instant when reduce_motion is enabled). */
  if (gv->graph != NULL) {
    for (guint i = 0; i < gv->graph->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      if (nd->birth_t < 1.0) {
        nd->birth_t = gv->reduce_motion ? 1.0 : MIN(nd->birth_t + 0.09, 1.0);
        if (nd->birth_t < 1.0)
          active = TRUE;
      }
    }
  }

  /* 2. Hover fade animation. */
  gdouble target_hf = (gv->hover_idx >= 0) ? 1.0 : 0.0;
  if (gv->reduce_motion) {
    gv->hover_fade = target_hf;
  } else if (fabs(gv->hover_fade - target_hf) > 0.02) {
    gv->hover_fade += (target_hf - gv->hover_fade) * 0.25;
    active = TRUE;
  } else {
    gv->hover_fade = target_hf;
  }

  /* 3. Smooth camera transition. */
  if (gv->animating_cam) {
    if (gv->reduce_motion) {
      gv->pan_x = gv->target_pan_x;
      gv->pan_y = gv->target_pan_y;
      gv->zoom = gv->target_zoom;
      gv->animating_cam = FALSE;
    } else {
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
  }

  /* 4. Slow ambient dust drift on High quality when window is focused & motion allowed. */
  if (needs_ambient_tick(gv)) {
    gv->time_sec += dt;
    active = TRUE;
  }

  queue_canvas_draw(gv);
  if (!active) {
    gv->tick_id = 0;
    gv->last_tick_us = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void ensure_tick(BtGraphView *gv)
{
  if (gv->canvas_stack == NULL || gv->tick_id != 0 || !gtk_widget_get_mapped(gv->canvas_stack))
    return;
  gv->last_tick_us = g_get_monotonic_time();
  gv->tick_id = gtk_widget_add_tick_callback(gv->canvas_stack, on_tick, gv, NULL);
}

static void stop_tick_if_idle(BtGraphView *gv)
{
  if (gv->tick_id == 0 || gv->canvas_stack == NULL)
    return;
  if (!needs_ambient_tick(gv) && !gv->animating_cam && !graph_has_pending_birth(gv->graph)) {
    gtk_widget_remove_tick_callback(gv->canvas_stack, gv->tick_id);
    gv->tick_id = 0;
    gv->last_tick_us = 0;
  }
}

static void fill_render_state(BtGraphView *gv, int w, int h, int scale, BtGraphRenderState *out)
{
  if (gv->needs_fit && gv->graph != NULL && gv->graph->nodes->len > 0 && w > 20 && h > 20) {
    compute_fit_camera(gv, w, h, &gv->pan_x, &gv->pan_y, &gv->zoom);
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
    gv->target_zoom = gv->zoom;
    gv->animating_cam = FALSE;
    gv->needs_fit = FALSE;
  }

  guint n = (gv->graph != NULL) ? gv->graph->nodes->len : 0u;
  memset(out, 0, sizeof(*out));
  out->graph = gv->graph;
  out->width = w;
  out->height = h;
  out->scale = MAX(scale, 1);
  out->pan_x = gv->pan_x;
  out->pan_y = gv->pan_y;
  out->zoom = gv->zoom;
  out->hover_idx = gv->hover_idx;
  out->selected_idx = gv->selected_idx;
  out->focus_idx = gv->focus_idx;
  out->hover_fade = gv->hover_fade;
  out->nbr_mask = gv->nbr_mask;
  out->nbr_mask_len = MIN(n, gv->nbr_mask_cap);
  out->hub_thresh = gv->hub_thresh;
  out->time_sec = gv->time_sec;
  out->quality = gv->quality;
  out->mode = BT_GRAPH_MODE_2D;
  out->reduce_motion = gv->reduce_motion;
}

static void on_gl_fill_state(BtGraphRenderState *out, gpointer ud)
{
  BtGraphView *gv = ud;
  fill_render_state(gv, out->width, out->height, out->scale, out);
}

static void on_gl_fallback(const gchar *reason, gpointer ud)
{
  BtGraphView *gv = ud;
  gv->active_backend = BT_GRAPH_BACKEND_CAIRO;
  if (gv->canvas_stack != NULL)
    gtk_stack_set_visible_child_name(GTK_STACK(gv->canvas_stack), "cairo");
  gchar *msg = g_strdup_printf("Graph: Cairo fallback (%s)", reason ? reason : "GL unavailable");
  emit_notice(gv, msg);
  g_free(msg);
  queue_canvas_draw(gv);
}

static void on_cairo_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
  BtGraphView *gv = ud;
  BtGraphRenderState st;
  fill_render_state(gv, w, h, gtk_widget_get_scale_factor(GTK_WIDGET(area)), &st);
  if (gv->cairo_renderer != NULL && gv->cairo_renderer->vt->render_cairo != NULL)
    gv->cairo_renderer->vt->render_cairo(gv->cairo_renderer, cr, &st);
}

static void screen_to_world(BtGraphView *gv, gdouble sx, gdouble sy, gdouble *wx, gdouble *wy)
{
  int w = gtk_widget_get_width(gv->canvas_stack);
  int h = gtk_widget_get_height(gv->canvas_stack);
  *wx = (sx - w * 0.5 - gv->pan_x) / gv->zoom;
  *wy = (sy - h * 0.5 - gv->pan_y) / gv->zoom;
}

/* ---------------------------------------------- pointer & toolbar UI -- */

static void on_click_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g;
  BtGraphView *gv = ud;
  if (gv->graph == NULL)
    return;
  gdouble wx = 0, wy = 0;
  screen_to_world(gv, x, y, &wx, &wy);
  gint hit = bt_graph_pick(gv->graph, wx, wy, 14.0 / gv->zoom);
  if (hit >= 0) {
    gv->selected_idx = hit;
    bt_graph_gl_mark_state_dirty(gv->gl);
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    if (n_press == 2) {
      gv->target_pan_x = -nd->x * gv->zoom;
      gv->target_pan_y = -nd->y * gv->zoom;
      gv->target_zoom = gv->zoom;
      gv->animating_cam = TRUE;
      ensure_tick(gv);
      return;
    }
    if (n_press == 1 && nd->relpath != NULL && !nd->broken && gv->on_open != NULL)
      gv->on_open(nd->relpath, nd->kind == BT_NODE_GHOST, gv->user_data);
    queue_canvas_draw(gv);
    return;
  }
  if (n_press != 1)
    return;
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
    queue_canvas_draw(gv);
    return;
  }
  if (gv->graph != NULL) {
    gdouble wx = 0, wy = 0;
    screen_to_world(gv, x, y, &wx, &wy);
    gint hit = bt_graph_pick(gv->graph, wx, wy, 14.0 / gv->zoom);
    if (hit != gv->hover_idx) {
      gv->hover_idx = hit;
      refresh_neighbor_mask(gv);
      if (gv->reduce_motion) {
        gv->hover_fade = (hit >= 0) ? 1.0 : 0.0;
        queue_canvas_draw(gv);
      } else {
        ensure_tick(gv);
      }
    }
  }
}

static void on_motion_leave(GtkEventControllerMotion *c, gpointer ud)
{
  (void)c;
  BtGraphView *gv = ud;
  if (gv->hover_idx != -1) {
    gv->hover_idx = -1;
    refresh_neighbor_mask(gv);
    if (gv->reduce_motion) {
      gv->hover_fade = 0.0;
      queue_canvas_draw(gv);
    } else {
      ensure_tick(gv);
    }
  }
}

static gboolean on_scroll(GtkEventControllerScroll *c, gdouble dx, gdouble dy, gpointer ud)
{
  (void)c; (void)dx;
  BtGraphView *gv = ud;
  gv->animating_cam = FALSE;
  gv->zoom = CLAMP(gv->zoom * (dy < 0 ? 1.12 : 0.89), 0.2, 4.0);
  queue_canvas_draw(gv);
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
  if (gv->canvas_stack == NULL)
    return;
  compute_fit_camera(gv, gtk_widget_get_width(gv->canvas_stack),
                     gtk_widget_get_height(gv->canvas_stack),
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

static void sync_toolbar_buttons(BtGraphView *gv)
{
  if (gv->btn_quality != NULL) {
    gboolean hi = (gv->quality == BT_GRAPH_QUALITY_HIGH);
    gtk_button_set_label(GTK_BUTTON(gv->btn_quality), hi ? "High" : "Low");
    if (hi)
      gtk_widget_add_css_class(gv->btn_quality, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_quality, "bt-pill-active");
  }
  if (gv->btn_motion != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_motion), gv->reduce_motion);
  }
}

static void on_btn_quality_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  bt_graphview_set_quality(gv, gv->quality == BT_GRAPH_QUALITY_HIGH ? BT_GRAPH_QUALITY_LOW
                                                                    : BT_GRAPH_QUALITY_HIGH);
}

static void on_btn_motion_toggled(GtkToggleButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  gboolean rm = gtk_toggle_button_get_active(b);
  if (rm != gv->reduce_motion)
    bt_graphview_set_reduce_motion(gv, rm);
}

static void on_toplevel_active_notify(GObject *obj, GParamSpec *pspec, gpointer ud)
{
  (void)pspec;
  BtGraphView *gv = ud;
  gv->window_active = gtk_window_is_active(GTK_WINDOW(obj));
  if (gv->window_active && needs_ambient_tick(gv))
    ensure_tick(gv);
  else
    stop_tick_if_idle(gv);
}

static void on_canvas_map(GtkWidget *area, gpointer ud)
{
  BtGraphView *gv = ud;
  GtkRoot *rt = gtk_widget_get_root(area);
  if (GTK_IS_WINDOW(rt) && gv->toplevel != GTK_WINDOW(rt)) {
    if (gv->toplevel != NULL && gv->toplevel_active_sig != 0)
      g_signal_handler_disconnect(gv->toplevel, gv->toplevel_active_sig);
    gv->toplevel = GTK_WINDOW(rt);
    gv->window_active = gtk_window_is_active(gv->toplevel);
    gv->toplevel_active_sig =
        g_signal_connect(gv->toplevel, "notify::is-active",
                         G_CALLBACK(on_toplevel_active_notify), gv);
  }
  if (gv->graph != NULL && (graph_has_pending_birth(gv->graph) || needs_ambient_tick(gv)))
    ensure_tick(gv);
  queue_canvas_draw(gv);
}

static void on_canvas_unmap(GtkWidget *area, gpointer ud)
{
  (void)area;
  BtGraphView *gv = ud;
  if (gv->tick_id != 0 && gv->canvas_stack != NULL) {
    gtk_widget_remove_tick_callback(gv->canvas_stack, gv->tick_id);
    gv->tick_id = 0;
    gv->last_tick_us = 0;
  }
}

static void on_canvas_destroy(GtkWidget *area, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->tick_id != 0) {
    gtk_widget_remove_tick_callback(area, gv->tick_id);
    gv->tick_id = 0;
  }
  if (gv->toplevel != NULL && gv->toplevel_active_sig != 0) {
    g_signal_handler_disconnect(gv->toplevel, gv->toplevel_active_sig);
    gv->toplevel_active_sig = 0;
    gv->toplevel = NULL;
  }
  gv->canvas_stack = NULL;
  gv->cairo_area = NULL;
  gv->overlay = NULL;
  gv->root = NULL;
  gv->btn_quality = NULL;
  gv->btn_motion = NULL;
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
  gv->window_active = TRUE;
  gv->quality = BT_GRAPH_QUALITY_HIGH;
  gv->hover_idx = -1;
  gv->selected_idx = -1;
  gv->focus_idx = -1;
  gv->on_open = on_open;
  gv->on_open_term = on_open_term;
  gv->user_data = user_data;
  gv->cairo_renderer = bt_graph_cairo_renderer_new();

  GtkSettings *settings = gtk_settings_get_default();
  if (settings != NULL) {
    gboolean anim = TRUE;
    g_object_get(settings, "gtk-enable-animations", &anim, NULL);
    gv->reduce_motion = !anim;
  }

  const gchar *env_be = g_getenv("BRAINTERM_GRAPH_BACKEND");
  gboolean force_cairo = (env_be != NULL && g_ascii_strcasecmp(env_be, "cairo") == 0);

  gv->cairo_area = gtk_drawing_area_new();
  gtk_widget_set_name(gv->cairo_area, "bt-graph-cairo");
  gtk_widget_set_hexpand(gv->cairo_area, TRUE);
  gtk_widget_set_vexpand(gv->cairo_area, TRUE);
  gtk_widget_set_size_request(gv->cairo_area, 200, 160);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(gv->cairo_area), on_cairo_draw, gv, NULL);

  gv->canvas_stack = gtk_stack_new();
  gtk_widget_set_name(gv->canvas_stack, "bt-graph-area");
  gtk_widget_set_hexpand(gv->canvas_stack, TRUE);
  gtk_widget_set_vexpand(gv->canvas_stack, TRUE);
  gtk_widget_set_size_request(gv->canvas_stack, 200, 160);
  gtk_stack_add_named(GTK_STACK(gv->canvas_stack), gv->cairo_area, "cairo");

  if (!force_cairo) {
    gv->gl = bt_graph_gl_new(on_gl_fill_state, on_gl_fallback, gv);
    gtk_stack_add_named(GTK_STACK(gv->canvas_stack), bt_graph_gl_widget(gv->gl), "gl");
    gtk_stack_set_visible_child_name(GTK_STACK(gv->canvas_stack), "gl");
    gv->active_backend = BT_GRAPH_BACKEND_GL;
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(gv->canvas_stack), "cairo");
    gv->active_backend = BT_GRAPH_BACKEND_CAIRO;
    emit_notice(gv, "Graph: Cairo fallback (forced via BRAINTERM_GRAPH_BACKEND=cairo)");
  }

  g_signal_connect(gv->canvas_stack, "map", G_CALLBACK(on_canvas_map), gv);
  g_signal_connect(gv->canvas_stack, "unmap", G_CALLBACK(on_canvas_unmap), gv);
  g_signal_connect(gv->canvas_stack, "destroy", G_CALLBACK(on_canvas_destroy), gv);

  GtkGesture *lclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lclick), GDK_BUTTON_PRIMARY);
  g_signal_connect(lclick, "pressed", G_CALLBACK(on_click_pressed), gv);
  g_signal_connect(lclick, "released", G_CALLBACK(on_click_released), gv);
  gtk_widget_add_controller(gv->canvas_stack, GTK_EVENT_CONTROLLER(lclick));

  GtkGesture *rclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect(rclick, "pressed", G_CALLBACK(on_right_click), gv);
  gtk_widget_add_controller(gv->canvas_stack, GTK_EVENT_CONTROLLER(rclick));

  GtkEventController *mot = gtk_event_controller_motion_new();
  g_signal_connect(mot, "motion", G_CALLBACK(on_motion), gv);
  g_signal_connect(mot, "leave", G_CALLBACK(on_motion_leave), gv);
  gtk_widget_add_controller(gv->canvas_stack, mot);

  GtkEventController *scr = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scr, "scroll", G_CALLBACK(on_scroll), gv);
  gtk_widget_add_controller(gv->canvas_stack, scr);

  gv->overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(gv->overlay, TRUE);
  gtk_widget_set_vexpand(gv->overlay, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(gv->overlay), gv->canvas_stack);

  /* Top-left graph toolbar: Quality (High/Low) & Reduce Motion toggles. */
  GtkWidget *mode_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_name(mode_bar, "bt-graph-toolbar");
  gtk_widget_add_css_class(mode_bar, "bt-graph-controls");
  gtk_widget_set_halign(mode_bar, GTK_ALIGN_START);
  gtk_widget_set_valign(mode_bar, GTK_ALIGN_START);
  gtk_widget_set_margin_top(mode_bar, 10);
  gtk_widget_set_margin_start(mode_bar, 10);

  gv->btn_quality = gtk_button_new_with_label("High");
  gtk_widget_add_css_class(gv->btn_quality, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_quality,
                              "Quality: High (glow halos, curved additive edges, dust) / Low (fast)");
  g_signal_connect(gv->btn_quality, "clicked", G_CALLBACK(on_btn_quality_clicked), gv);

  gv->btn_motion = gtk_toggle_button_new_with_label("Reduce motion");
  gtk_widget_add_css_class(gv->btn_motion, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_motion,
                              "Reduce motion: stops drifting dust and ambient animations");
  g_signal_connect(gv->btn_motion, "toggled", G_CALLBACK(on_btn_motion_toggled), gv);

  gtk_box_append(GTK_BOX(mode_bar), gv->btn_quality);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_motion);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), mode_bar);
  sync_toolbar_buttons(gv);

  /* Top-right zoom / fit / reset overlay controls. */
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

  /* Calm empty state when graph has 0 nodes. */
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
  if (gv->tick_id != 0 && gv->canvas_stack != NULL) {
    gtk_widget_remove_tick_callback(gv->canvas_stack, gv->tick_id);
    gv->tick_id = 0;
  }
  if (gv->toplevel != NULL && gv->toplevel_active_sig != 0) {
    g_signal_handler_disconnect(gv->toplevel, gv->toplevel_active_sig);
    gv->toplevel_active_sig = 0;
  }
  if (gv->cairo_renderer != NULL && gv->cairo_renderer->vt->destroy != NULL)
    gv->cairo_renderer->vt->destroy(gv->cairo_renderer);
  bt_graph_gl_free(gv->gl);
  g_free(gv->pending_notice);
  g_free(gv->nbr_mask);
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
  gv->selected_idx = -1;
  gv->focus_idx = -1;
  gv->hover_fade = 0.0;
  gv->needs_fit = TRUE;
  gv->hub_thresh = bt_graph_hub_degree_threshold(graph);
  bt_graph_gl_set_graph(gv->gl, graph);

  gboolean has_nodes = (gv->graph != NULL && gv->graph->nodes->len > 0);
  if (has_nodes) {
    ensure_nbr_mask(gv, gv->graph->nodes->len);
    memset(gv->nbr_mask, BT_NBR_NONE, gv->graph->nodes->len);
  }
  if (gv->root != NULL)
    gtk_stack_set_visible_child_name(GTK_STACK(gv->root), has_nodes ? "canvas" : "empty");

  if (gv->graph != NULL && animate_growth && !gv->reduce_motion) {
    for (guint i = 0; i < gv->graph->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      nd->birth_t = 0.05;
    }
    ensure_tick(gv);
  } else if (needs_ambient_tick(gv)) {
    ensure_tick(gv);
  } else {
    stop_tick_if_idle(gv);
  }
  queue_canvas_draw(gv);
}

void bt_graphview_set_dark(BtGraphView *gv, gboolean dark)
{
  if (gv == NULL)
    return;
  gv->dark_mode = dark;
  queue_canvas_draw(gv);
}

void bt_graphview_set_quality(BtGraphView *gv, BtGraphQuality quality)
{
  if (gv == NULL)
    return;
  gv->quality = quality;
  sync_toolbar_buttons(gv);
  if (needs_ambient_tick(gv))
    ensure_tick(gv);
  else
    stop_tick_if_idle(gv);
  queue_canvas_draw(gv);
}

BtGraphQuality bt_graphview_get_quality(const BtGraphView *gv)
{
  return gv ? gv->quality : BT_GRAPH_QUALITY_HIGH;
}

void bt_graphview_set_reduce_motion(BtGraphView *gv, gboolean reduce_motion)
{
  if (gv == NULL)
    return;
  gv->reduce_motion = reduce_motion;
  sync_toolbar_buttons(gv);
  if (reduce_motion)
    stop_tick_if_idle(gv);
  else if (needs_ambient_tick(gv))
    ensure_tick(gv);
  queue_canvas_draw(gv);
}

gboolean bt_graphview_get_reduce_motion(const BtGraphView *gv)
{
  return gv ? gv->reduce_motion : FALSE;
}

void bt_graphview_set_notice_cb(BtGraphView *gv, BtGraphNoticeFn on_notice, gpointer user_data)
{
  if (gv == NULL)
    return;
  gv->on_notice = on_notice;
  gv->notice_ud = user_data;
  if (gv->on_notice != NULL && gv->pending_notice != NULL)
    gv->on_notice(gv->pending_notice, gv->notice_ud);
}

gboolean bt_graphview_has_graph(const BtGraphView *gv)
{
  return gv != NULL && gv->graph != NULL;
}

gdouble bt_graphview_min_birth_t(const BtGraphView *gv)
{
  if (gv == NULL || gv->graph == NULL || gv->graph->nodes->len == 0)
    return 1.0;
  gdouble m = 1.0;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (nd->birth_t < m)
      m = nd->birth_t;
  }
  return m;
}
