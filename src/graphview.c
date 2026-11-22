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
  BtGraphLabelCand *lbl_cands;
  BtGraphLabelCand lbl_placed[64];
  BtGraphCoreObstacle *obs_scratch;
  guint lbl_scratch_cap;
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
    g_free(p->lbl_cands);
    g_free(p->obs_scratch);
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

  gint focus_idx = (st->hover_idx >= 0)
                       ? st->hover_idx
                       : ((st->focus_idx >= 0) ? st->focus_idx : st->selected_idx);
  gboolean has_focus = (focus_idx >= 0 && (guint)focus_idx < g->nodes->len);
  gdouble hf = has_focus ? CLAMP(st->hover_fade, 0.0, 1.0) : 0.0;
  if (st->hover_idx < 0 && (st->focus_idx >= 0 || st->selected_idx >= 0))
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
      gdouble core_w = bt_graph_screen_core_radius(nd->radius, zoom) / zoom;
      gdouble halo_r = MIN(core_w * (is_foc ? 2.8 : (is_sel ? 2.6 : 2.4)), 30.0 / zoom);
      gdouble halo_a = is_foc ? (0.40 + 0.38 * hf)
                              : ((nbr == BT_NBR_NEIGHBOR || is_sel) ? 0.45 : (0.28 * alpha));
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
    gdouble rad = bt_graph_screen_core_radius(nd->radius, zoom) / zoom;

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

  cairo_restore(cr);

  /* Pass 3: Greedy screen-space culled Pango labels (shared bt_graph_place_labels). */
  if (!very_heavy || has_focus || st->selected_idx >= 0) {
    if (g->nodes->len > p->lbl_scratch_cap) {
      g_free(p->lbl_cands);
      g_free(p->obs_scratch);
      p->lbl_cands = g_new0(BtGraphLabelCand, g->nodes->len);
      p->obs_scratch = g_new0(BtGraphCoreObstacle, g->nodes->len);
      p->lbl_scratch_cap = g->nodes->len;
    }

    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Inter, Segoe UI, Sans 10");
    pango_font_description_set_size(fd, (gint)(10.5 * PANGO_SCALE));
    pango_layout_set_font_description(pl, fd);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(pl, 220 * PANGO_SCALE);

    guint cand_n = 0;
    guint obs_n = 0;
    for (guint i = 0; i < g->nodes->len; i++) {
      const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      if (!nd->visible)
        continue;
      gdouble nx, ny;
      node_pos(nd, &nx, &ny);
      float sx = (float)(width * 0.5 + st->pan_x + nx * zoom);
      float sy = (float)(height * 0.5 + st->pan_y + ny * zoom);
      float core_px = (float)bt_graph_screen_core_radius(nd->radius, zoom);
      if (sx + core_px >= -20.0f && sx - core_px <= (float)width + 20.0f &&
          sy + core_px >= -20.0f && sy - core_px <= (float)height + 20.0f) {
        p->obs_scratch[obs_n].cx = sx;
        p->obs_scratch[obs_n].cy = sy;
        p->obs_scratch[obs_n].r = core_px;
        p->obs_scratch[obs_n].node_idx = i;
        obs_n++;
      }

      guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
      gboolean is_sel = ((gint)i == st->selected_idx);
      gboolean is_kbd = ((gint)i == st->focus_idx);
      gboolean is_hov = ((gint)i == st->hover_idx) || (nbr == BT_NBR_FOCUS);
      gdouble la = bt_graph_label_alpha(nd, nbr, is_sel, is_kbd, st->hub_thresh, zoom,
                                        has_focus, hf, g->nodes->len);
      if (la < 0.06)
        continue;

      pango_layout_set_text(pl, nd->label, -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      if (tw <= 0 || th <= 0)
        continue;
      float bw = (float)(tw + 8);
      float bh = (float)(th + 4);
      if (sx + bw * 0.5f < 0.0f || sx - bw * 0.5f > (float)width ||
          sy + bh + 24.0f < 0.0f || sy - 24.0f > (float)height)
        continue;

      guint8 prio = 1u;
      if (is_kbd) prio = 5u;
      else if (is_sel) prio = 4u;
      else if (is_hov) prio = 3u;
      else if (nbr == BT_NBR_NEIGHBOR) prio = 2u;

      BtGraphLabelCand *c = &p->lbl_cands[cand_n++];
      c->node_idx = i;
      c->x = floorf(sx - bw * 0.5f);
      c->y = floorf(sy + core_px + 5.0f);
      c->w = bw;
      c->h = bh;
      c->alpha = (float)la;
      c->degree = nd->degree;
      c->priority = prio;
      c->always_show = (is_kbd || is_sel || is_hov);
      c->is_neighbor = (nbr == BT_NBR_NEIGHBOR && !c->always_show);
      c->pill = c->always_show;
    }

    guint max_labels = bt_graph_label_cap_for_zoom(zoom);
    guint placed = bt_graph_place_labels(p->lbl_cands, cand_n, p->obs_scratch, obs_n,
                                         max_labels, 12u, p->lbl_placed, 64u);

    for (guint k = 0; k < placed; k++) {
      const BtGraphLabelCand *lp = &p->lbl_placed[placed - 1u - k];
      const BtGraphNode *nd = g_ptr_array_index(g->nodes, lp->node_idx);
      pango_layout_set_text(pl, nd->label, -1);

      draw_rrect(cr, (gdouble)lp->x, (gdouble)lp->y, (gdouble)lp->w, (gdouble)lp->h, 4.0);
      if (lp->pill) {
        set_tok(cr, BT_COL_SURFACE_2, 0.92 * (gdouble)lp->alpha);
        cairo_fill_preserve(cr);
        set_tok(cr, (lp->priority >= 4u) ? BT_COL_ACCENT : BT_COL_BORDER_STRONG,
                0.9 * (gdouble)lp->alpha);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
      } else {
        set_tok(cr, BT_COL_GRAPH_BG, 0.70 * (gdouble)lp->alpha);
        cairo_fill(cr);
      }
      set_tok(cr, lp->pill ? BT_COL_TEXT_PRIMARY : BT_COL_TEXT_SECONDARY, (gdouble)lp->alpha);
      cairo_move_to(cr, (gdouble)lp->x + 4.0, (gdouble)lp->y + 2.0);
      pango_cairo_show_layout(cr, pl);
    }

    if (has_stem) {
      pango_layout_set_text(pl, "sessions", -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      gdouble sx_stem = width * 0.5 + st->pan_x + (stem_min_x + stem_max_x) * 0.5 * zoom;
      gdouble sy_stem = height * 0.5 + st->pan_y + 224.0 * zoom;
      set_tok(cr, BT_COL_TEXT_MUTED, 0.72 * dim_factor);
      cairo_move_to(cr, sx_stem - (gdouble)tw * 0.5, sy_stem);
      pango_cairo_show_layout(cr, pl);
    }

    pango_font_description_free(fd);
    g_object_unref(pl);
  }
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

typedef struct {
  gint refcnt;
  gboolean alive;
  BtGraphView *gv;
} BtGraphAlive;

static BtGraphAlive *alive_new(BtGraphView *gv)
{
  BtGraphAlive *a = g_new0(BtGraphAlive, 1);
  a->refcnt = 1;
  a->alive = TRUE;
  a->gv = gv;
  return a;
}

static BtGraphAlive *alive_ref(BtGraphAlive *a)
{
  g_atomic_int_inc(&a->refcnt);
  return a;
}

static void alive_unref(BtGraphAlive *a)
{
  if (a != NULL && g_atomic_int_dec_and_test(&a->refcnt))
    g_free(a);
}

struct BtGraphView {
  GtkWidget *root;         /* GtkStack: "canvas" | "empty" */
  GtkWidget *overlay;      /* GtkOverlay wrapping canvas_stack + controls + legend */
  GtkWidget *canvas_stack; /* GtkStack ("bt-graph-area"): "gl" | "cairo" */
  GtkWidget *cairo_area;   /* GtkDrawingArea fallback */
  BtGraphGL *gl;           /* GtkGLArea backend */
  GtkWidget *btn_mode;
  GtkWidget *btn_quality;
  GtkWidget *btn_pulse;
  GtkWidget *btn_motion;
  GtkWidget *btn_tags;
  GtkWidget *btn_ghosts;
  GtkWidget *legend_lbl;
  GtkWindow *toplevel;
  gulong toplevel_active_sig;

  BtGraphAlive *alive;
  BtGraph *graph;
  BtGraphVec3 *pts3d;
  guint pts3d_count;
  guint layout3d_gen;

  BtGraphRenderer *cairo_renderer;
  BtGraphBackend active_backend;
  gchar *pending_notice;
  guint8 *nbr_mask;
  guint nbr_mask_cap;
  guint hub_thresh;

  /* 2D camera */
  gdouble pan_x, pan_y, zoom;
  gdouble target_pan_x, target_pan_y, target_zoom;
  gboolean animating_cam;
  gboolean needs_fit;

  /* 3D orbit camera */
  gdouble yaw, pitch, dist3d;
  gdouble target_yaw, target_pitch, target_dist3d;
  gboolean animating_3d;
  gboolean user_interacted_3d;

  gboolean dark_mode;
  gboolean window_active;
  BtGraphQuality quality;
  BtGraphMode mode;
  gboolean pulse_enabled;
  gboolean reduce_motion;
  gboolean show_tags;
  gboolean show_ghosts;

  gboolean dragging;
  gboolean drag_moved;
  gboolean has_mouse;
  gdouble last_mouse_x, last_mouse_y;
  gdouble drag_start_x, drag_start_y;
  gdouble pan_start_x, pan_start_y;
  gdouble yaw_start, pitch_start;

  gint hover_idx;
  gint selected_idx;
  gint focus_idx;
  gdouble hover_fade;
  gdouble time_sec;
  gdouble pulse_phase;
  gint64 last_tick_us;
  guint tick_id;

  GtkWidget *ctx_popover;
  gint ctx_hit_idx;
  gchar *ctx_relpath;
  gboolean ctx_is_ghost;

  BtGraphOpenNoteFn on_open;
  BtGraphOpenTermFn on_open_term;
  BtGraphRenameNoteFn on_rename;
  BtGraphNoticeFn on_notice;
  BtGraphPrefsChangedFn on_prefs;
  gpointer user_data;
  gpointer rename_ud;
  gpointer notice_ud;
  gpointer prefs_ud;
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
  gint foc = (gv->hover_idx >= 0)
                 ? gv->hover_idx
                 : ((gv->focus_idx >= 0) ? gv->focus_idx : gv->selected_idx);
  bt_graph_compute_neighbors(gv->graph, foc, gv->nbr_mask, n);
  bt_graph_gl_mark_state_dirty(gv->gl);
}

/* --------------------- worker-thread 3D layout calculation -- */

typedef struct {
  BtGraphAlive *alive;
  guint gen;
  BtGraph *snapshot; /* lightweight node-only clone for worker thread */
  BtGraphVec3 *pts;
  guint count;
} BtLayout3DJob;

static gboolean on_layout3d_done(gpointer ud)
{
  BtLayout3DJob *job = ud;
  if (job->alive->alive && job->alive->gv != NULL && job->alive->gv->layout3d_gen == job->gen) {
    BtGraphView *gv = job->alive->gv;
    g_free(gv->pts3d);
    gv->pts3d = job->pts;
    gv->pts3d_count = job->count;
    job->pts = NULL;
    bt_graph_gl_set_layout3d(gv->gl, gv->pts3d, gv->pts3d_count);
    queue_canvas_draw(gv);
  }
  g_free(job->pts);
  if (job->snapshot != NULL) {
    g_ptr_array_unref(job->snapshot->nodes);
    g_free(job->snapshot);
  }
  alive_unref(job->alive);
  g_free(job);
  return G_SOURCE_REMOVE;
}

static gpointer layout3d_worker(gpointer ud)
{
  BtLayout3DJob *job = ud;
  if (job->count > 0 && job->snapshot != NULL) {
    job->pts = g_new0(BtGraphVec3, job->count);
    bt_graph_compute_3d_layout(job->snapshot, job->pts, job->count);
  }
  g_idle_add(on_layout3d_done, job);
  return NULL;
}

static void schedule_layout3d_worker(BtGraphView *gv)
{
  gv->layout3d_gen++;
  if (gv->graph == NULL || gv->graph->nodes == NULL || gv->graph->nodes->len == 0) {
    g_clear_pointer(&gv->pts3d, g_free);
    gv->pts3d_count = 0;
    bt_graph_gl_set_layout3d(gv->gl, NULL, 0);
    return;
  }

  guint n = gv->graph->nodes->len;
  BtGraph *snap = g_new0(BtGraph, 1);
  snap->nodes = g_ptr_array_new_full(n, g_free);
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *src = g_ptr_array_index(gv->graph->nodes, i);
    BtGraphNode *dst = g_new0(BtGraphNode, 1);
    dst->x = src->x;
    dst->y = src->y;
    dst->hue = src->hue;
    dst->kind = src->kind;
    dst->degree = src->degree;
    dst->visible = src->visible;
    g_ptr_array_add(snap->nodes, dst);
  }

  BtLayout3DJob *job = g_new0(BtLayout3DJob, 1);
  job->alive = alive_ref(gv->alive);
  job->gen = gv->layout3d_gen;
  job->snapshot = snap;
  job->count = n;
  GThread *th = g_thread_new("bt-graph-3d", layout3d_worker, job);
  g_thread_unref(th);
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

static gboolean needs_idle_orbit(const BtGraphView *gv)
{
  return gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL &&
         !gv->user_interacted_3d && !gv->reduce_motion && gv->window_active &&
         gv->graph != NULL && gv->graph->nodes->len > 0;
}

static gboolean needs_pulse_tick(const BtGraphView *gv)
{
  return gv->pulse_enabled && !gv->reduce_motion && gv->window_active &&
         gv->active_backend == BT_GRAPH_BACKEND_GL && gv->selected_idx >= 0 &&
         gv->graph != NULL && gv->graph->nodes->len > 0;
}

static gboolean needs_ambient_tick(const BtGraphView *gv)
{
  if (gv->graph == NULL || gv->graph->nodes->len == 0)
    return FALSE;
  if (gv->reduce_motion || !gv->window_active)
    return FALSE;
  if (needs_idle_orbit(gv) || needs_pulse_tick(gv))
    return TRUE;
  if (gv->quality != BT_GRAPH_QUALITY_HIGH)
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

  /* 1. Node growth animation. */
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

  /* 2. Hover / pinned selection fade animation. */
  gdouble target_hf = (gv->hover_idx >= 0 || gv->focus_idx >= 0 || gv->selected_idx >= 0) ? 1.0 : 0.0;
  if (gv->reduce_motion) {
    gv->hover_fade = target_hf;
  } else if (fabs(gv->hover_fade - target_hf) > 0.02) {
    gv->hover_fade += (target_hf - gv->hover_fade) * 0.25;
    active = TRUE;
  } else {
    gv->hover_fade = target_hf;
  }

  /* 3. Smooth 2D camera transition. */
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

  /* 4. Smooth 3D orbit camera transition. */
  if (gv->animating_3d) {
    if (gv->reduce_motion) {
      gv->yaw = gv->target_yaw;
      gv->pitch = gv->target_pitch;
      gv->dist3d = gv->target_dist3d;
      gv->animating_3d = FALSE;
    } else {
      gv->yaw += (gv->target_yaw - gv->yaw) * 0.22;
      gv->pitch += (gv->target_pitch - gv->pitch) * 0.22;
      gv->dist3d += (gv->target_dist3d - gv->dist3d) * 0.22;
      if (fabs(gv->yaw - gv->target_yaw) < 0.005 &&
          fabs(gv->pitch - gv->target_pitch) < 0.005 &&
          fabs(gv->dist3d - gv->target_dist3d) < 0.01) {
        gv->yaw = gv->target_yaw;
        gv->pitch = gv->target_pitch;
        gv->dist3d = gv->target_dist3d;
        gv->animating_3d = FALSE;
      } else {
        active = TRUE;
      }
    }
  }

  /* 5. Gentle 3D idle rotation (stops on user interaction / unfocus / Reduce Motion). */
  if (needs_idle_orbit(gv) && !gv->animating_3d) {
    gv->yaw += dt * 0.14;
    if (gv->yaw > 2.0 * M_PI)
      gv->yaw -= 2.0 * M_PI;
    active = TRUE;
  }

  /* 6. Subtle traveling edge pulse from selected node. */
  if (needs_pulse_tick(gv)) {
    gv->pulse_phase = fmod(gv->pulse_phase + dt * 0.55, 1.0);
    active = TRUE;
  }

  /* 7. Ambient dust drift. */
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
  if (!needs_ambient_tick(gv) && !gv->animating_cam && !gv->animating_3d &&
      !graph_has_pending_birth(gv->graph)) {
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
  out->yaw = gv->yaw;
  out->pitch = gv->pitch;
  out->dist3d = gv->dist3d;
  out->hover_idx = gv->hover_idx;
  out->selected_idx = gv->selected_idx;
  out->focus_idx = gv->focus_idx;
  out->hover_fade = gv->hover_fade;
  out->nbr_mask = gv->nbr_mask;
  out->nbr_mask_len = MIN(n, gv->nbr_mask_cap);
  out->hub_thresh = gv->hub_thresh;
  out->time_sec = gv->time_sec;
  out->pulse_phase = gv->pulse_phase;
  out->quality = gv->quality;
  out->mode = (gv->active_backend == BT_GRAPH_BACKEND_GL) ? gv->mode : BT_GRAPH_MODE_2D;
  out->reduce_motion = gv->reduce_motion;
  out->pulse_enabled = gv->pulse_enabled;
}

static void sync_toolbar_buttons(BtGraphView *gv);

static void on_gl_fill_state(BtGraphRenderState *out, gpointer ud)
{
  BtGraphView *gv = ud;
  fill_render_state(gv, out->width, out->height, out->scale, out);
}

static void on_gl_fallback(const gchar *reason, gpointer ud)
{
  BtGraphView *gv = ud;
  gv->active_backend = BT_GRAPH_BACKEND_CAIRO;
  gv->mode = BT_GRAPH_MODE_2D;
  if (gv->canvas_stack != NULL)
    gtk_stack_set_visible_child_name(GTK_STACK(gv->canvas_stack), "cairo");
  sync_toolbar_buttons(gv);
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

static gint pick_node_at(BtGraphView *gv, gdouble sx, gdouble sy)
{
  if (gv->graph == NULL || gv->graph->nodes->len == 0 || gv->canvas_stack == NULL)
    return -1;
  if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL &&
      gv->pts3d != NULL && gv->pts3d_count > 0) {
    int w = gtk_widget_get_width(gv->canvas_stack);
    int h = gtk_widget_get_height(gv->canvas_stack);
    if (w <= 1 || h <= 1)
      return -1;
    float mvp[16];
    bt_graph_build_orbit_mvp(gv->yaw, gv->pitch, MAX(gv->dist3d, 1.2),
                             (gdouble)w / (gdouble)h, mvp);
    gint best = -1;
    gdouble best_score = 1e9;
    guint n = MIN(gv->graph->nodes->len, gv->pts3d_count);
    for (guint i = 0; i < n; i++) {
      const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      if (!nd->visible)
        continue;
      gdouble px = 0.0, py = 0.0, pw = 0.0;
      if (!bt_graph_project_3d(mvp, &gv->pts3d[i], (gdouble)w, (gdouble)h, &px, &py, &pw))
        continue;
      gdouble d2 = (px - sx) * (px - sx) + (py - sy) * (py - sy);
      gdouble hr = MAX(nd->radius * (2.2 / MAX(pw, 0.5)), 12.0);
      if (d2 <= hr * hr && pw < best_score) {
        best_score = pw;
        best = (gint)i;
      }
    }
    return best;
  }

  gdouble wx = 0.0, wy = 0.0;
  screen_to_world(gv, sx, sy, &wx, &wy);
  return bt_graph_pick(gv->graph, wx, wy, 14.0 / gv->zoom);
}

/* ---------------------------------------------- pointer & toolbar UI -- */

static void center_on_node(BtGraphView *gv, gint hit)
{
  if (gv == NULL || gv->graph == NULL || hit < 0 || (guint)hit >= gv->graph->nodes->len)
    return;
  const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
  if (gv->mode == BT_GRAPH_MODE_3D && gv->pts3d != NULL && (guint)hit < gv->pts3d_count) {
    gv->user_interacted_3d = TRUE;
    const BtGraphVec3 *p = &gv->pts3d[hit];
    gv->target_yaw = atan2((gdouble)p->x, (gdouble)p->z);
    gdouble horiz = hypot((gdouble)p->x, (gdouble)p->z);
    gv->target_pitch = CLAMP(atan2((gdouble)p->y, MAX(horiz, 0.05)), -1.25, 1.25);
    gv->target_dist3d = MIN(gv->dist3d, 2.35);
    gv->animating_3d = TRUE;
  } else {
    gv->target_pan_x = -nd->x * gv->zoom;
    gv->target_pan_y = -nd->y * gv->zoom;
    gv->target_zoom = gv->zoom;
    gv->animating_cam = TRUE;
  }
  ensure_tick(gv);
}

static void show_graph_context_menu(BtGraphView *gv, gdouble x, gdouble y);

static void clear_selection(BtGraphView *gv)
{
  if (gv == NULL || gv->selected_idx < 0)
    return;
  gv->selected_idx = -1;
  refresh_neighbor_mask(gv);
  sync_toolbar_buttons(gv);
  if (gv->reduce_motion) {
    gv->hover_fade = (gv->hover_idx >= 0 || gv->focus_idx >= 0) ? 1.0 : 0.0;
    queue_canvas_draw(gv);
  } else {
    ensure_tick(gv);
  }
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode,
                               GdkModifierType state, gpointer ud)
{
  (void)c; (void)keycode; (void)state;
  BtGraphView *gv = ud;
  if (keyval == GDK_KEY_Escape && gv->selected_idx >= 0) {
    clear_selection(gv);
    return TRUE;
  }
  return FALSE;
}

static void on_click_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g;
  BtGraphView *gv = ud;
  if (gv->canvas_stack != NULL)
    gtk_widget_grab_focus(gv->canvas_stack);
  if (gv->graph == NULL)
    return;
  gint hit = pick_node_at(gv, x, y);
  if (hit >= 0) {
    gv->selected_idx = hit;
    gv->pulse_phase = 0.0;
    refresh_neighbor_mask(gv);
    sync_toolbar_buttons(gv);
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    if (n_press == 2) {
      center_on_node(gv, hit);
      return;
    }
    if (n_press == 1 && nd->relpath != NULL && !nd->broken && gv->on_open != NULL)
      gv->on_open(nd->relpath, nd->kind == BT_NODE_GHOST, gv->user_data);
    if (gv->reduce_motion) {
      gv->hover_fade = 1.0;
      queue_canvas_draw(gv);
    } else {
      ensure_tick(gv);
    }
    return;
  }
  if (n_press != 1)
    return;
  gv->animating_cam = FALSE;
  gv->animating_3d = FALSE;
  gv->dragging = TRUE;
  gv->drag_moved = FALSE;
  gv->drag_start_x = x;
  gv->drag_start_y = y;
  gv->pan_start_x = gv->pan_x;
  gv->pan_start_y = gv->pan_y;
  gv->yaw_start = gv->yaw;
  gv->pitch_start = gv->pitch;
  if (gv->mode == BT_GRAPH_MODE_3D)
    gv->user_interacted_3d = TRUE;
}

static void on_click_released(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g; (void)n_press; (void)x; (void)y;
  BtGraphView *gv = ud;
  gboolean was_clean_bg_click = gv->dragging && !gv->drag_moved;
  gv->dragging = FALSE;
  gv->drag_moved = FALSE;
  if (was_clean_bg_click && gv->selected_idx >= 0)
    clear_selection(gv);
}

static void on_right_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g;
  BtGraphView *gv = ud;
  if (n_press != 1 || gv->graph == NULL)
    return;
  show_graph_context_menu(gv, x, y);
}

static void on_motion(GtkEventControllerMotion *c, gdouble x, gdouble y, gpointer ud)
{
  (void)c;
  BtGraphView *gv = ud;
  gv->has_mouse = TRUE;
  gv->last_mouse_x = x;
  gv->last_mouse_y = y;
  if (gv->dragging) {
    if (hypot(x - gv->drag_start_x, y - gv->drag_start_y) > 4.0)
      gv->drag_moved = TRUE;
    if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL) {
      gv->user_interacted_3d = TRUE;
      gv->yaw = gv->yaw_start - (x - gv->drag_start_x) * 0.0075;
      gv->pitch = CLAMP(gv->pitch_start + (y - gv->drag_start_y) * 0.0075, -1.35, 1.35);
    } else {
      gv->pan_x = gv->pan_start_x + (x - gv->drag_start_x);
      gv->pan_y = gv->pan_start_y + (y - gv->drag_start_y);
    }
    queue_canvas_draw(gv);
    return;
  }
  if (gv->graph != NULL) {
    gint hit = pick_node_at(gv, x, y);
    if (hit != gv->hover_idx) {
      gv->hover_idx = hit;
      refresh_neighbor_mask(gv);
      if (gv->reduce_motion) {
        gv->hover_fade = (hit >= 0 || gv->selected_idx >= 0 || gv->focus_idx >= 0) ? 1.0 : 0.0;
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
  gv->has_mouse = FALSE;
  if (gv->hover_idx != -1) {
    gv->hover_idx = -1;
    refresh_neighbor_mask(gv);
    if (gv->reduce_motion) {
      gv->hover_fade = (gv->selected_idx >= 0 || gv->focus_idx >= 0) ? 1.0 : 0.0;
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
  gv->animating_3d = FALSE;
  if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->user_interacted_3d = TRUE;
    gv->dist3d = CLAMP(gv->dist3d * (dy < 0 ? 0.90 : 1.11), 1.35, 5.5);
  } else {
    gdouble vp_w = (gdouble)gtk_widget_get_width(gv->canvas_stack);
    gdouble vp_h = (gdouble)gtk_widget_get_height(gv->canvas_stack);
    gdouble mx = gv->has_mouse ? gv->last_mouse_x : (vp_w * 0.5);
    gdouble my = gv->has_mouse ? gv->last_mouse_y : (vp_h * 0.5);
    gdouble nz = gv->zoom * (dy < 0 ? 1.12 : 0.89);
    bt_graph_zoom_at_point(&gv->pan_x, &gv->pan_y, &gv->zoom, nz, mx, my, vp_w, vp_h);
  }
  queue_canvas_draw(gv);
  return TRUE;
}

static void on_btn_zoom_in(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->user_interacted_3d = TRUE;
    gv->target_yaw = gv->yaw;
    gv->target_pitch = gv->pitch;
    gv->target_dist3d = CLAMP(gv->dist3d * 0.82, 1.35, 5.5);
    gv->animating_3d = TRUE;
  } else {
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
    gv->target_zoom = CLAMP(gv->zoom * 1.25, 0.2, 4.0);
    gv->animating_cam = TRUE;
  }
  ensure_tick(gv);
}

static void on_btn_zoom_out(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->user_interacted_3d = TRUE;
    gv->target_yaw = gv->yaw;
    gv->target_pitch = gv->pitch;
    gv->target_dist3d = CLAMP(gv->dist3d * 1.22, 1.35, 5.5);
    gv->animating_3d = TRUE;
  } else {
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
    gv->target_zoom = CLAMP(gv->zoom * 0.8, 0.2, 4.0);
    gv->animating_cam = TRUE;
  }
  ensure_tick(gv);
}

static void on_btn_fit(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (gv->canvas_stack == NULL)
    return;
  if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->target_yaw = 0.35;
    gv->target_pitch = 0.20;
    gv->target_dist3d = 2.8;
    gv->animating_3d = TRUE;
    gv->user_interacted_3d = FALSE;
  } else {
    compute_fit_camera(gv, gtk_widget_get_width(gv->canvas_stack),
                       gtk_widget_get_height(gv->canvas_stack),
                       &gv->target_pan_x, &gv->target_pan_y, &gv->target_zoom);
    gv->animating_cam = TRUE;
  }
  ensure_tick(gv);
}

static void on_btn_reset(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (gv->mode == BT_GRAPH_MODE_3D && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->target_yaw = 0.35;
    gv->target_pitch = 0.20;
    gv->target_dist3d = 2.8;
    gv->animating_3d = TRUE;
    gv->user_interacted_3d = FALSE;
  } else {
    gv->target_pan_x = 0.0;
    gv->target_pan_y = 0.0;
    gv->target_zoom = 1.0;
    gv->animating_cam = TRUE;
  }
  ensure_tick(gv);
}

static void sync_toolbar_buttons(BtGraphView *gv)
{
  gboolean gl_ok = (gv->active_backend == BT_GRAPH_BACKEND_GL);
  if (gv->btn_mode != NULL) {
    gboolean is3d = gl_ok && (gv->mode == BT_GRAPH_MODE_3D);
    gtk_button_set_label(GTK_BUTTON(gv->btn_mode), is3d ? "3D" : "2D");
    gtk_widget_set_sensitive(gv->btn_mode, gl_ok);
    gtk_widget_set_tooltip_text(
        gv->btn_mode, gl_ok ? "Toggle 2D planar / 3D cortex orbit view"
                            : "3D orbit view requires OpenGL (running Cairo fallback)");
    if (is3d)
      gtk_widget_add_css_class(gv->btn_mode, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_mode, "bt-pill-active");
  }
  if (gv->btn_quality != NULL) {
    gboolean hi = (gv->quality == BT_GRAPH_QUALITY_HIGH);
    gtk_button_set_label(GTK_BUTTON(gv->btn_quality), hi ? "High" : "Low");
    if (hi)
      gtk_widget_add_css_class(gv->btn_quality, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_quality, "bt-pill-active");
  }
  if (gv->btn_pulse != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_pulse), gv->pulse_enabled);
    gtk_widget_set_sensitive(gv->btn_pulse, gl_ok && !gv->reduce_motion);
  }
  if (gv->btn_motion != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_motion), gv->reduce_motion);
  }
  if (gv->btn_tags != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_tags), gv->show_tags);
  }
  if (gv->btn_ghosts != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_ghosts), gv->show_ghosts);
  }
}

static void update_legend(BtGraphView *gv)
{
  if (gv == NULL || gv->legend_lbl == NULL)
    return;
  gchar *markup = g_strdup_printf(
      "<span foreground='#E6E8EC'>●</span> Note  "
      "<span foreground='#8B7CF6'>●</span> AI  "
      "<span foreground='%s'>● Tag%s</span>  "
      "<span foreground='%s'>◌ Ghost%s</span>  "
      "<span foreground='#4ADE80'>●</span> Folder",
      gv->show_tags ? "#FBBF24" : "#666D79",
      gv->show_tags ? "" : " (off)",
      gv->show_ghosts ? "#9AA1AD" : "#666D79",
      gv->show_ghosts ? "" : " (off)");
  gtk_label_set_markup(GTK_LABEL(gv->legend_lbl), markup);
  g_free(markup);
}

static void apply_kind_visibility(BtGraphView *gv, gboolean upload_gl)
{
  if (gv == NULL || gv->graph == NULL)
    return;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (nd->kind == BT_NODE_TAG)
      nd->visible = gv->show_tags;
    else if (nd->kind == BT_NODE_GHOST)
      nd->visible = gv->show_ghosts;
  }
  if (gv->hover_idx >= 0 && (guint)gv->hover_idx < gv->graph->nodes->len &&
      !((const BtGraphNode *)g_ptr_array_index(gv->graph->nodes, (guint)gv->hover_idx))->visible)
    gv->hover_idx = -1;
  if (gv->selected_idx >= 0 && (guint)gv->selected_idx < gv->graph->nodes->len &&
      !((const BtGraphNode *)g_ptr_array_index(gv->graph->nodes, (guint)gv->selected_idx))->visible)
    gv->selected_idx = -1;
  if (gv->focus_idx >= 0 && (guint)gv->focus_idx < gv->graph->nodes->len &&
      !((const BtGraphNode *)g_ptr_array_index(gv->graph->nodes, (guint)gv->focus_idx))->visible)
    gv->focus_idx = -1;

  gv->hub_thresh = bt_graph_hub_degree_threshold(gv->graph);
  if (upload_gl)
    bt_graph_gl_set_graph(gv->gl, gv->graph);
  refresh_neighbor_mask(gv);
}

static void on_btn_mode_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  bt_graphview_set_mode(gv, gv->mode == BT_GRAPH_MODE_3D ? BT_GRAPH_MODE_2D : BT_GRAPH_MODE_3D);
}

static void on_btn_quality_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  bt_graphview_set_quality(gv, gv->quality == BT_GRAPH_QUALITY_HIGH ? BT_GRAPH_QUALITY_LOW
                                                                    : BT_GRAPH_QUALITY_HIGH);
}

static void on_btn_pulse_toggled(GtkToggleButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  gboolean en = gtk_toggle_button_get_active(b);
  if (en != gv->pulse_enabled)
    bt_graphview_set_pulse(gv, en);
}

static void on_btn_motion_toggled(GtkToggleButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  gboolean rm = gtk_toggle_button_get_active(b);
  if (rm != gv->reduce_motion)
    bt_graphview_set_reduce_motion(gv, rm);
}

static void on_btn_tags_toggled(GtkToggleButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  gboolean en = gtk_toggle_button_get_active(b);
  if (en != gv->show_tags) {
    bt_graphview_set_show_tags(gv, en);
    if (gv->on_prefs != NULL)
      gv->on_prefs(gv->show_tags, gv->show_ghosts, gv->prefs_ud);
  }
}

static void on_btn_ghosts_toggled(GtkToggleButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  gboolean en = gtk_toggle_button_get_active(b);
  if (en != gv->show_ghosts) {
    bt_graphview_set_show_ghosts(gv, en);
    if (gv->on_prefs != NULL)
      gv->on_prefs(gv->show_tags, gv->show_ghosts, gv->prefs_ud);
  }
}

/* ------------------------------------------- right-click context menu -- */

static void dismiss_ctx_popover(BtGraphView *gv)
{
  if (gv->ctx_popover != NULL) {
    GtkWidget *old = gv->ctx_popover;
    gv->ctx_popover = NULL;
    gtk_popover_popdown(GTK_POPOVER(old));
    gtk_widget_unparent(old);
  }
  g_clear_pointer(&gv->ctx_relpath, g_free);
  gv->ctx_hit_idx = -1;
  gv->ctx_is_ghost = FALSE;
}

static void on_ctx_closed(GtkPopover *pop, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->ctx_popover == GTK_WIDGET(pop)) {
    gv->ctx_popover = NULL;
    gtk_widget_unparent(GTK_WIDGET(pop));
  }
}

static void on_gctx_open(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gchar *rel = g_strdup(gv->ctx_relpath);
  gboolean ghost = gv->ctx_is_ghost;
  dismiss_ctx_popover(gv);
  if (rel != NULL && gv->on_open != NULL)
    gv->on_open(rel, ghost, gv->user_data);
  g_free(rel);
}

static void on_gctx_rename(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gchar *rel = g_strdup(gv->ctx_relpath);
  dismiss_ctx_popover(gv);
  if (rel != NULL && gv->on_rename != NULL)
    gv->on_rename(rel, gv->rename_ud);
  g_free(rel);
}

static void on_gctx_center(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  dismiss_ctx_popover(gv);
  center_on_node(gv, hit);
}

static void on_gctx_term(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gchar *rel = g_strdup(gv->ctx_relpath);
  dismiss_ctx_popover(gv);
  if (rel != NULL && gv->on_open_term != NULL) {
    gchar *dir = g_path_get_dirname(rel);
    const gchar *rel_dir = (dir != NULL && g_strcmp0(dir, ".") != 0) ? dir : NULL;
    gv->on_open_term(rel_dir, gv->user_data);
    g_free(dir);
  }
  g_free(rel);
}

static void on_gctx_fit(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  dismiss_ctx_popover(gv);
  on_btn_fit(NULL, gv);
}

static void on_gctx_reset(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  dismiss_ctx_popover(gv);
  on_btn_reset(NULL, gv);
}

static void on_gctx_toggle_mode(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  dismiss_ctx_popover(gv);
  on_btn_mode_clicked(NULL, gv);
}

static GtkWidget *make_gctx_btn(const gchar *label, const gchar *accel, GCallback cb,
                                BtGraphView *gv)
{
  GtkWidget *btn = gtk_button_new();
  gtk_widget_add_css_class(btn, "bt-ctx-item");
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *lbl = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_widget_set_hexpand(lbl, TRUE);
  gtk_box_append(GTK_BOX(row), lbl);
  if (accel != NULL && accel[0] != '\0') {
    GtkWidget *acc = gtk_label_new(accel);
    gtk_label_set_xalign(GTK_LABEL(acc), 1.0f);
    gtk_widget_add_css_class(acc, "bt-ctx-accel");
    gtk_box_append(GTK_BOX(row), acc);
  }
  gtk_button_set_child(GTK_BUTTON(btn), row);
  g_signal_connect(btn, "clicked", cb, gv);
  return btn;
}

static GtkWidget *make_gctx_sep(void)
{
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(sep, "bt-ctx-sep");
  return sep;
}

static void show_graph_context_menu(BtGraphView *gv, gdouble x, gdouble y)
{
  if (gv->canvas_stack == NULL)
    return;
  dismiss_ctx_popover(gv);

  gint hit = pick_node_at(gv, x, y);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_add_css_class(box, "bt-ctx-menu");
  gtk_widget_set_focusable(box, TRUE);

  if (hit >= 0 && gv->graph != NULL && (guint)hit < gv->graph->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    gv->selected_idx = hit;
    gv->ctx_hit_idx = hit;
    gv->ctx_relpath = g_strdup(nd->relpath);
    gv->ctx_is_ghost = (nd->kind == BT_NODE_GHOST);
    bt_graph_gl_mark_state_dirty(gv->gl);
    queue_canvas_draw(gv);

    gboolean has_note = (nd->relpath != NULL && !nd->broken);
    gboolean is_real_note = has_note && (nd->kind != BT_NODE_GHOST);

    if (has_note && gv->on_open != NULL)
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn(gv->ctx_is_ghost ? "Create Note" : "Open Note", NULL,
                                   G_CALLBACK(on_gctx_open), gv));
    gtk_box_append(GTK_BOX(box),
                   make_gctx_btn("Center on Node", NULL, G_CALLBACK(on_gctx_center), gv));
    if (is_real_note && (gv->on_open_term != NULL || gv->on_rename != NULL))
      gtk_box_append(GTK_BOX(box), make_gctx_sep());
    if (is_real_note && gv->on_open_term != NULL)
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn("Open in Terminal", NULL, G_CALLBACK(on_gctx_term), gv));
    if (is_real_note && gv->on_rename != NULL)
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn("Rename…", "F2", G_CALLBACK(on_gctx_rename), gv));
  } else {
    gtk_box_append(GTK_BOX(box), make_gctx_btn("Fit Graph", NULL, G_CALLBACK(on_gctx_fit), gv));
    gtk_box_append(GTK_BOX(box), make_gctx_btn("Reset View", NULL, G_CALLBACK(on_gctx_reset), gv));
    if (gv->active_backend == BT_GRAPH_BACKEND_GL) {
      gtk_box_append(GTK_BOX(box), make_gctx_sep());
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn(gv->mode == BT_GRAPH_MODE_3D ? "Switch to 2D" : "Switch to 3D",
                                   NULL, G_CALLBACK(on_gctx_toggle_mode), gv));
    }
  }

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_add_css_class(pop, "bt-ctx-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
  gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
  gtk_widget_set_halign(pop, GTK_ALIGN_START);
  gtk_popover_set_child(GTK_POPOVER(pop), box);
  gtk_widget_set_parent(pop, gv->canvas_stack);

  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  g_signal_connect(pop, "closed", G_CALLBACK(on_ctx_closed), gv);

  gv->ctx_popover = pop;
  gtk_popover_popup(GTK_POPOVER(pop));
  gtk_widget_grab_focus(box);
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
  dismiss_ctx_popover(gv);
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
  gv->btn_mode = NULL;
  gv->btn_quality = NULL;
  gv->btn_pulse = NULL;
  gv->btn_motion = NULL;
  gv->btn_tags = NULL;
  gv->btn_ghosts = NULL;
  gv->legend_lbl = NULL;
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
  gv->alive = alive_new(gv);
  gv->zoom = 1.0;
  gv->target_zoom = 1.0;
  gv->yaw = 0.35;
  gv->pitch = 0.20;
  gv->dist3d = 2.8;
  gv->target_yaw = gv->yaw;
  gv->target_pitch = gv->pitch;
  gv->target_dist3d = gv->dist3d;
  gv->dark_mode = TRUE;
  gv->window_active = TRUE;
  gv->quality = BT_GRAPH_QUALITY_HIGH;
  gv->mode = BT_GRAPH_MODE_2D;
  gv->pulse_enabled = FALSE;
  gv->hover_idx = -1;
  gv->selected_idx = -1;
  gv->focus_idx = -1;
  gv->ctx_hit_idx = -1;
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
  gtk_widget_set_focusable(gv->canvas_stack, TRUE);
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

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), gv);
  gtk_widget_add_controller(gv->canvas_stack, key);

  gv->overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(gv->overlay, TRUE);
  gtk_widget_set_vexpand(gv->overlay, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(gv->overlay), gv->canvas_stack);

  /* Top-left graph toolbar: 2D/3D, Quality (High/Low), Pulse, Reduce Motion. */
  GtkWidget *mode_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_name(mode_bar, "bt-graph-toolbar");
  gtk_widget_add_css_class(mode_bar, "bt-graph-controls");
  gtk_widget_set_halign(mode_bar, GTK_ALIGN_START);
  gtk_widget_set_valign(mode_bar, GTK_ALIGN_START);
  gtk_widget_set_margin_top(mode_bar, 10);
  gtk_widget_set_margin_start(mode_bar, 10);

  gv->btn_mode = gtk_button_new_with_label("2D");
  gtk_widget_add_css_class(gv->btn_mode, "bt-graph-pill");
  g_signal_connect(gv->btn_mode, "clicked", G_CALLBACK(on_btn_mode_clicked), gv);

  gv->btn_quality = gtk_button_new_with_label("High");
  gtk_widget_add_css_class(gv->btn_quality, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_quality,
                              "Quality: High (bloom, curved additive edges, dust) / Low (fast)");
  g_signal_connect(gv->btn_quality, "clicked", G_CALLBACK(on_btn_quality_clicked), gv);

  gv->btn_pulse = gtk_toggle_button_new_with_label("Pulse");
  gtk_widget_add_css_class(gv->btn_pulse, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_pulse,
                              "Subtle light ripples along edges from the selected node");
  g_signal_connect(gv->btn_pulse, "toggled", G_CALLBACK(on_btn_pulse_toggled), gv);

  gv->btn_motion = gtk_toggle_button_new_with_label("Reduce motion");
  gtk_widget_add_css_class(gv->btn_motion, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_motion,
                              "Reduce motion: stops dust, pulses, and 3D idle rotation");
  g_signal_connect(gv->btn_motion, "toggled", G_CALLBACK(on_btn_motion_toggled), gv);

  gv->btn_tags = gtk_toggle_button_new_with_label("Tags");
  gtk_widget_add_css_class(gv->btn_tags, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_tags, "Show or hide tag nodes and tag edges");
  g_signal_connect(gv->btn_tags, "toggled", G_CALLBACK(on_btn_tags_toggled), gv);

  gv->btn_ghosts = gtk_toggle_button_new_with_label("Ghosts");
  gtk_widget_add_css_class(gv->btn_ghosts, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_ghosts,
                              "Show or hide unresolved [[wiki-link]] ghost nodes");
  g_signal_connect(gv->btn_ghosts, "toggled", G_CALLBACK(on_btn_ghosts_toggled), gv);

  gtk_box_append(GTK_BOX(mode_bar), gv->btn_mode);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_quality);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_tags);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_ghosts);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_pulse);
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

  gv->legend_lbl = gtk_label_new(NULL);
  gtk_widget_add_css_class(gv->legend_lbl, "bt-graph-legend");
  gtk_widget_set_halign(gv->legend_lbl, GTK_ALIGN_START);
  gtk_widget_set_valign(gv->legend_lbl, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(gv->legend_lbl, 10);
  gtk_widget_set_margin_start(gv->legend_lbl, 10);
  gtk_widget_set_can_target(gv->legend_lbl, FALSE);
  update_legend(gv);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), gv->legend_lbl);

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
  dismiss_ctx_popover(gv);
  if (gv->alive != NULL) {
    gv->alive->alive = FALSE;
    gv->alive->gv = NULL;
    alive_unref(gv->alive);
    gv->alive = NULL;
  }
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
  g_free(gv->pts3d);
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
  gv->ctx_hit_idx = -1;
  gv->hover_fade = 0.0;
  gv->needs_fit = TRUE;
  apply_kind_visibility(gv, FALSE);
  bt_graph_gl_set_graph(gv->gl, graph);
  schedule_layout3d_worker(gv);

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

void bt_graphview_set_mode(BtGraphView *gv, BtGraphMode mode)
{
  if (gv == NULL)
    return;
  if (mode == BT_GRAPH_MODE_3D && gv->active_backend != BT_GRAPH_BACKEND_GL)
    mode = BT_GRAPH_MODE_2D;
  gv->mode = mode;
  if (mode == BT_GRAPH_MODE_3D)
    gv->user_interacted_3d = FALSE;
  sync_toolbar_buttons(gv);
  if (needs_ambient_tick(gv))
    ensure_tick(gv);
  else
    stop_tick_if_idle(gv);
  queue_canvas_draw(gv);
}

BtGraphMode bt_graphview_get_mode(const BtGraphView *gv)
{
  return gv ? gv->mode : BT_GRAPH_MODE_2D;
}

void bt_graphview_set_pulse(BtGraphView *gv, gboolean enabled)
{
  if (gv == NULL)
    return;
  gv->pulse_enabled = enabled;
  gv->pulse_phase = 0.0;
  sync_toolbar_buttons(gv);
  if (needs_ambient_tick(gv))
    ensure_tick(gv);
  else
    stop_tick_if_idle(gv);
  queue_canvas_draw(gv);
}

gboolean bt_graphview_get_pulse(const BtGraphView *gv)
{
  return gv ? gv->pulse_enabled : FALSE;
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

void bt_graphview_set_show_tags(BtGraphView *gv, gboolean show_tags)
{
  if (gv == NULL)
    return;
  gv->show_tags = show_tags ? TRUE : FALSE;
  sync_toolbar_buttons(gv);
  update_legend(gv);
  apply_kind_visibility(gv, TRUE);
  queue_canvas_draw(gv);
}

gboolean bt_graphview_get_show_tags(const BtGraphView *gv)
{
  return gv ? gv->show_tags : FALSE;
}

void bt_graphview_set_show_ghosts(BtGraphView *gv, gboolean show_ghosts)
{
  if (gv == NULL)
    return;
  gv->show_ghosts = show_ghosts ? TRUE : FALSE;
  sync_toolbar_buttons(gv);
  update_legend(gv);
  apply_kind_visibility(gv, TRUE);
  queue_canvas_draw(gv);
}

gboolean bt_graphview_get_show_ghosts(const BtGraphView *gv)
{
  return gv ? gv->show_ghosts : FALSE;
}

void bt_graphview_set_prefs_cb(BtGraphView *gv, BtGraphPrefsChangedFn on_prefs, gpointer user_data)
{
  if (gv == NULL)
    return;
  gv->on_prefs = on_prefs;
  gv->prefs_ud = user_data;
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

void bt_graphview_set_rename_cb(BtGraphView *gv, BtGraphRenameNoteFn on_rename, gpointer user_data)
{
  if (gv == NULL)
    return;
  gv->on_rename = on_rename;
  gv->rename_ud = user_data;
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
