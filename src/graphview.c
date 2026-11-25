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
#include "graph_palette.h"
#include "graph_state.h"
#include "graph_timeline.h"
#include "theme.h"
#include "ui/graphgl.h"
#include "ui/graphview_renderer.h"

#include <cjson/cJSON.h>
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
  guint last_label_count;
  guint last_overlap_count;
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

static guint32 node_color(const BtGraphNode *nd)
{
  if (nd != NULL && nd->color_rgb != 0u)
    return nd->color_rgb;
  BtNodeKind k = nd ? nd->kind : BT_NODE_HUMAN_NOTE;
  switch (k) {
  case BT_NODE_HUMAN_NOTE:  return 0xBFD7FFu;
  case BT_NODE_AI_NOTE:     return BT_COL_NODE_AI;
  case BT_NODE_TAG:         return BT_COL_NODE_TAG;
  case BT_NODE_FOLDER:      return 0xFFC857u;
  case BT_NODE_IMPORT_ROOT: return BT_COL_NODE_FOLDER;
  case BT_NODE_SESSION:     return BT_COL_TEXT_SECONDARY;
  case BT_NODE_GHOST:
  default:                  return BT_COL_TEXT_MUTED;
  }
}

static guint32 kind_default_color(BtNodeKind k)
{
  BtGraphNode tmp = {.kind = k, .color_rgb = 0u};
  return node_color(&tmp);
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
    p->glow[i] = bake_glow_sprite(kind_default_color(kinds[i]));
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
  gdouble dim_target = st->focus_isolated ? 0.08 : 0.25;
  gdouble dim_factor = has_focus ? (1.0 - (1.0 - dim_target) * hf) : 1.0;
  gboolean use_curved_edges = (high && g->edges->len <= 600u);
  gboolean lod_collapse = (g->nodes->len > 2000u && zoom < 0.6);

  /* Pass 1: base edges — faint folder tethers always, wikilinks only if show_links. */
  cairo_save(cr);
  cairo_set_antialias(cr, (heavy || !high) ? CAIRO_ANTIALIAS_NONE : CAIRO_ANTIALIAS_GOOD);
  cairo_set_line_width(cr, (high ? 1.0 : 0.9) / zoom);
  set_tok(cr, BT_COL_GRAPH_EDGE_DIM, (high ? 0.18 : 0.14) * dim_factor);

  for (guint i = 0; i < g->edges->len; i++) {
    const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
    if (e->kind != BT_EDGE_CONTAINS && !st->show_links)
      continue;
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
    guint32 col = node_color(nd);
    gdouble rad = bt_graph_screen_core_radius(nd->radius, zoom) / zoom;

    if (nd->kind == BT_NODE_FOLDER) {
      set_tok(cr, col, 0.92 * alpha);
      cairo_set_line_width(cr, 1.8 / zoom);
      cairo_arc(cr, nx, ny, rad, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_arc(cr, nx, ny, rad * 0.42, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    } else if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT) {
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
      /* White hot-center ONLY for hovered/selected nodes. */
      if ((is_foc || is_sel) && rad >= 3.0) {
        set_tok(cr, BT_COL_TEXT_PRIMARY, 0.72 * alpha);
        cairo_arc(cr, nx, ny, rad * 0.45, 0.0, 2.0 * M_PI);
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
      guint8 nbr = (st->nbr_mask != NULL && i < st->nbr_mask_len) ? st->nbr_mask[i] : BT_NBR_NONE;
      gboolean is_sel = ((gint)i == st->selected_idx);
      gboolean is_kbd = ((gint)i == st->focus_idx);
      gboolean is_hov = ((gint)i == st->hover_idx) || (nbr == BT_NBR_FOCUS);
      gboolean is_active_node =
          has_focus ? (nbr != BT_NBR_NONE || is_sel || is_kbd)
                    : ((st->hub_thresh > 0u && nd->degree >= st->hub_thresh) ||
                       g->nodes->len <= 32u || is_sel || is_kbd);

      if (is_active_node &&
          sx + core_px >= -20.0f && sx - core_px <= (float)width + 20.0f &&
          sy + core_px >= -20.0f && sy - core_px <= (float)height + 20.0f) {
        p->obs_scratch[obs_n].cx = sx;
        p->obs_scratch[obs_n].cy = sy;
        p->obs_scratch[obs_n].r = core_px;
        p->obs_scratch[obs_n].node_idx = i;
        obs_n++;
      }

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
      c->anchor_x = sx;
      c->anchor_y = sy;
      c->anchor_r = core_px;
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
    p->last_label_count = placed;
    guint ov_cnt = 0;
    for (guint i = 0; i < placed; i++) {
      for (guint j = i + 1; j < placed; j++) {
        const BtGraphLabelCand *a = &p->lbl_placed[i];
        const BtGraphLabelCand *b = &p->lbl_placed[j];
        if (a->x < b->x + b->w && a->x + a->w > b->x &&
            a->y < b->y + b->h && a->y + a->h > b->y) {
          ov_cnt++;
        }
      }
    }
    p->last_overlap_count = ov_cnt;

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

#define BT_FRAME_HIST_CAP 256u

struct BtGraphView {
  GtkWidget *root;         /* GtkStack: "canvas" | "empty" */
  GtkWidget *overlay;      /* GtkOverlay wrapping canvas_stack + controls + legend */
  GtkWidget *canvas_stack; /* GtkStack ("bt-graph-area"): "gl" | "cairo" */
  GtkWidget *cairo_area;   /* GtkDrawingArea fallback */
  BtGraphGL *gl;           /* GtkGLArea backend */
  GtkWidget *btn_2d;
  GtkWidget *btn_3d;
  GtkWidget *btn_galaxy;
  GtkWidget *btn_timeline;
  GtkWidget *tl_controls_box;
  GtkWidget *btn_tl_organic;
  GtkWidget *btn_tl_kept;
  GtkWidget *btn_tl_lane_folder;
  GtkWidget *btn_tl_lane_type;
  GtkWidget *btn_q_low;
  GtkWidget *btn_q_high;
  GtkWidget *btn_pulse;
  GtkWidget *btn_motion;
  GtkWidget *btn_tags;
  GtkWidget *btn_ghosts;
  GtkWidget *btn_links;
  GtkWidget *btn_col_folder;
  GtkWidget *btn_col_type;
  GtkWidget *legend_box;
  GtkWidget *legend_folders_box;
  GtkWidget *legend_lbl;
  GtkWidget *mode_badge_lbl;
  GtkWidget *hover_lbl;
  GtkWidget *debug_lbl;
  GtkWindow *toplevel;
  gulong toplevel_active_sig;
  gdouble gal_time;

  BtGraphAlive *alive;
  BtGraph *graph;
  BtGraphVec3 *pts3d;
  BtGraphVec3 *pts3d_from;
  BtGraphVec3 *pts3d_target;
  gdouble morph_t;
  gboolean morphing_3d;
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
  gboolean show_links;
  gboolean color_by_folder;
  BtFolderColors *folder_colors;
  gchar *vault_root_path;
  gchar *isolated_folder;
  gchar *color_target_folder;
  gboolean focus_isolated;
  guint focus_hops;
  gdouble spread_str;

  /* Timeline view state (Phase 4) */
  BtTimelineLayout *timeline;
  BtTimelineLaneBy tl_lane_by;
  gboolean tl_kept;
  gdouble tl_kept_t;
  gint64 tl_view_min;
  gint64 tl_view_max;
  gboolean tl_brushing;
  gdouble tl_brush_x0, tl_brush_x1;
  gint64 tl_brush_t0, tl_brush_t1;
  int tl_w, tl_h;
  gboolean tl_dirty;
  gint64 tl_pan_start_min, tl_pan_start_max;

  /* Exact spatial-hash picking & overlapping stack cycling */
  BtPickCand stack_cands[16];
  guint stack_count;
  guint stack_total;
  guint stack_idx;
  gdouble stack_x, stack_y;
  BtScreenNode *pick_scratch;
  guint pick_scratch_cap;

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

  /* Frame-time debug overlay metrics (from GdkFrameClock, zero per-frame alloc) */
  gboolean debug_overlay;
  gint64 frame_ts_us[BT_FRAME_HIST_CAP];
  gdouble frame_dt_ms[BT_FRAME_HIST_CAP];
  guint frame_hist_head;
  guint frame_hist_count;
  gint64 prev_frame_clock_us;
  gint64 last_debug_refresh_us;

  GtkWidget *ctx_popover;
  gint ctx_hit_idx;
  gchar *ctx_relpath;
  gboolean ctx_is_ghost;

  BtGraphOpenNoteFn on_open;
  BtGraphOpenTermFn on_open_term;
  BtGraphRenameNoteFn on_rename;
  BtGraphSelectNoteFn on_select;
  BtGraphNoticeFn on_notice;
  BtGraphPrefsChangedFn on_prefs;
  gpointer user_data;
  gpointer rename_ud;
  gpointer select_ud;
  gpointer notice_ud;
  gpointer prefs_ud;
};

static void queue_canvas_draw(BtGraphView *gv)
{
  if (gv == NULL)
    return;
  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    if (gv->cairo_area != NULL)
      gtk_widget_queue_draw(gv->cairo_area);
    return;
  }
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
  gint foc = (gv->focus_isolated && gv->selected_idx >= 0)
                 ? gv->selected_idx
                 : ((gv->hover_idx >= 0)
                        ? gv->hover_idx
                        : ((gv->focus_idx >= 0) ? gv->focus_idx : gv->selected_idx));
  guint hops = (gv->focus_isolated && gv->focus_hops >= 2u) ? 2u : 1u;
  bt_graph_compute_neighbors_hops(gv->graph, foc, hops, gv->nbr_mask, n);
  bt_graph_gl_mark_state_dirty(gv->gl);
}

/* --------------------- worker-thread 3D layout calculation -- */

static inline gboolean mode_is_3d_orbit(BtGraphMode m)
{
  return m == BT_GRAPH_MODE_3D || m == BT_GRAPH_MODE_GALAXY;
}

typedef struct {
  BtGraphAlive *alive;
  guint gen;
  BtGraphMode mode;
  BtGraph *snapshot; /* lightweight node-only clone for worker thread */
  BtGraphVec3 *pts;
  guint count;
} BtLayout3DJob;

static void ensure_tick(BtGraphView *gv);

static gboolean on_layout3d_done(gpointer ud)
{
  BtLayout3DJob *job = ud;
  if (job->alive->alive && job->alive->gv != NULL && job->alive->gv->layout3d_gen == job->gen) {
    BtGraphView *gv = job->alive->gv;
    if (gv->pts3d != NULL && gv->pts3d_count == job->count && !gv->reduce_motion &&
        mode_is_3d_orbit(gv->mode)) {
      /* Animate smoothly (~700ms) from previous 3D positions to new layout! */
      g_free(gv->pts3d_from);
      g_free(gv->pts3d_target);
      gv->pts3d_from = g_memdup2(gv->pts3d, job->count * sizeof(BtGraphVec3));
      gv->pts3d_target = job->pts;
      job->pts = NULL;
      gv->morph_t = 0.0;
      gv->morphing_3d = TRUE;
      ensure_tick(gv);
    } else {
      g_free(gv->pts3d);
      g_clear_pointer(&gv->pts3d_from, g_free);
      g_clear_pointer(&gv->pts3d_target, g_free);
      gv->pts3d = job->pts;
      gv->pts3d_count = job->count;
      gv->morphing_3d = FALSE;
      gv->morph_t = 1.0;
      job->pts = NULL;
      bt_graph_gl_set_layout3d(gv->gl, gv->pts3d, gv->pts3d_count);
    }
    queue_canvas_draw(gv);
  }
  g_free(job->pts);
  if (job->snapshot != NULL) {
    for (guint i = 0; i < job->snapshot->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(job->snapshot->nodes, i);
      g_free(nd->id);
      g_free(nd->relpath);
      g_free(nd->top_folder);
    }
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
    if (job->mode == BT_GRAPH_MODE_GALAXY)
      bt_graph_compute_galaxy_layout(job->snapshot, 4u, job->pts, job->count);
    else
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
    g_clear_pointer(&gv->pts3d_from, g_free);
    g_clear_pointer(&gv->pts3d_target, g_free);
    gv->pts3d_count = 0;
    gv->morphing_3d = FALSE;
    bt_graph_gl_set_layout3d(gv->gl, NULL, 0);
    return;
  }

  guint n = gv->graph->nodes->len;
  BtGraph *snap = g_new0(BtGraph, 1);
  snap->nodes = g_ptr_array_new_full(n, g_free);
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *src = g_ptr_array_index(gv->graph->nodes, i);
    BtGraphNode *dst = g_new0(BtGraphNode, 1);
    dst->id = g_strdup(src->id);
    dst->relpath = g_strdup(src->relpath);
    dst->top_folder = g_strdup(src->top_folder);
    dst->x = src->x;
    dst->y = src->y;
    dst->hue = src->hue;
    dst->kind = src->kind;
    dst->degree = src->degree;
    dst->parent_idx = src->parent_idx;
    dst->child_count = src->child_count;
    dst->visible = src->visible;
    g_ptr_array_add(snap->nodes, dst);
  }

  BtLayout3DJob *job = g_new0(BtLayout3DJob, 1);
  job->alive = alive_ref(gv->alive);
  job->gen = gv->layout3d_gen;
  job->mode = gv->mode;
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
  return mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL &&
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

static void count_visible_graph_stats(const BtGraphView *gv, guint *vis_nodes_out,
                                      guint *tot_nodes_out, guint *vis_edges_out,
                                      guint *tot_edges_out)
{
  guint vn = 0, tn = 0, ve = 0, te = 0;
  if (gv != NULL && gv->graph != NULL) {
    tn = gv->graph->nodes ? gv->graph->nodes->len : 0u;
    for (guint i = 0; i < tn; i++) {
      const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      if (nd->visible)
        vn++;
    }
    te = gv->graph->edges ? gv->graph->edges->len : 0u;
    for (guint i = 0; i < te; i++) {
      const BtGraphEdge *e = &g_array_index(gv->graph->edges, BtGraphEdge, i);
      if (e->src_idx < tn && e->dst_idx < tn) {
        const BtGraphNode *a = g_ptr_array_index(gv->graph->nodes, e->src_idx);
        const BtGraphNode *b = g_ptr_array_index(gv->graph->nodes, e->dst_idx);
        if (a->visible && b->visible)
          ve++;
      }
    }
  }
  if (vis_nodes_out) *vis_nodes_out = vn;
  if (tot_nodes_out) *tot_nodes_out = tn;
  if (vis_edges_out) *vis_edges_out = ve;
  if (tot_edges_out) *tot_edges_out = te;
}

static guint current_drawn_label_count(const BtGraphView *gv)
{
  if (gv == NULL)
    return 0u;
  if (gv->active_backend == BT_GRAPH_BACKEND_GL && gv->gl != NULL)
    return bt_graph_gl_last_label_count(gv->gl);
  if (gv->cairo_renderer != NULL && gv->cairo_renderer->priv != NULL)
    return ((const BtCairoPriv *)gv->cairo_renderer->priv)->last_label_count;
  return 0u;
}

static void record_frame_clock_sample(BtGraphView *gv, gint64 fc_us)
{
  if (fc_us <= 0)
    return;
  if (gv->prev_frame_clock_us > 0 && fc_us > gv->prev_frame_clock_us) {
    gdouble dt_ms = (gdouble)(fc_us - gv->prev_frame_clock_us) / 1000.0;
    if (dt_ms > 0.0 && dt_ms < 500.0) {
      gv->frame_ts_us[gv->frame_hist_head] = fc_us;
      gv->frame_dt_ms[gv->frame_hist_head] = dt_ms;
      gv->frame_hist_head = (gv->frame_hist_head + 1u) % BT_FRAME_HIST_CAP;
      if (gv->frame_hist_count < BT_FRAME_HIST_CAP)
        gv->frame_hist_count++;
    }
  }
  gv->prev_frame_clock_us = fc_us;
}

static void update_debug_overlay(BtGraphView *gv, gint64 now_fc_us)
{
  if (gv == NULL || gv->debug_lbl == NULL || !gv->debug_overlay)
    return;

  if (now_fc_us <= 0 && gv->canvas_stack != NULL) {
    GdkFrameClock *fc = gtk_widget_get_frame_clock(gv->canvas_stack);
    if (fc != NULL)
      now_fc_us = gdk_frame_clock_get_frame_time(fc);
  }
  if (now_fc_us <= 0)
    now_fc_us = gv->prev_frame_clock_us;

  gdouble dts[BT_FRAME_HIST_CAP];
  guint cnt = 0;
  gdouble sum = 0.0;
  gdouble max_dt = 0.0;
  gint64 cutoff = (now_fc_us > 1000000) ? (now_fc_us - 1000000) : 0;

  for (guint i = 0; i < gv->frame_hist_count; i++) {
    if (gv->frame_ts_us[i] >= cutoff && gv->frame_ts_us[i] <= now_fc_us) {
      gdouble d = gv->frame_dt_ms[i];
      dts[cnt++] = d;
      sum += d;
      if (d > max_dt)
        max_dt = d;
    }
  }

  /* Stack shell sort for exact p95 (zero heap alloc). */
  for (guint gap = cnt / 2u; gap > 0u; gap /= 2u) {
    for (guint i = gap; i < cnt; i++) {
      gdouble tmp = dts[i];
      guint j = i;
      while (j >= gap && dts[j - gap] > tmp) {
        dts[j] = dts[j - gap];
        j -= gap;
      }
      dts[j] = tmp;
    }
  }

  gdouble avg_ms = (cnt > 0u) ? (sum / (gdouble)cnt) : 0.0;
  gdouble p95_ms = (cnt > 0u) ? dts[(guint)((gdouble)(cnt - 1u) * 0.95)] : 0.0;
  gdouble fps = (gv->tick_id != 0 && avg_ms > 0.0) ? (1000.0 / avg_ms) : 0.0;

  guint vn = 0, tn = 0, ve = 0, te = 0;
  count_visible_graph_stats(gv, &vn, &tn, &ve, &te);
  guint lbls = current_drawn_label_count(gv);

  gboolean is_gl = (gv->active_backend == BT_GRAPH_BACKEND_GL && gv->gl != NULL &&
                    bt_graph_gl_is_ready(gv->gl));
  const gchar *be = is_gl ? "GL" : "Cairo";
  const gchar *gl_ver = is_gl ? bt_graph_gl_version(gv->gl) : "N/A";
  const gchar *gpu = is_gl ? bt_graph_gl_renderer(gv->gl) : "CPU (Cairo fallback)";
  const gchar *qual = (gv->quality == BT_GRAPH_QUALITY_HIGH) ? "High" : "Low";
  const gchar *mode = (is_gl && gv->mode == BT_GRAPH_MODE_GALAXY)
                          ? "Galaxy"
                          : ((is_gl && gv->mode == BT_GRAPH_MODE_3D) ? "3D" : "2D");
  const gchar *tick_st = (gv->tick_id != 0) ? "running" : "idle";

  char buf[512];
  g_snprintf(buf, sizeof(buf),
             "FPS %5.1f | frame avg %.2f / p95 %.2f / max %.2f ms | tick: %s\n"
             "%s %s | %s\n"
             "%s | %s | nodes %u/%u | edges %u/%u | labels %u",
             fps, avg_ms, p95_ms, max_dt, tick_st,
             be, gl_ver, gpu,
             mode, qual, vn, tn, ve, te, lbls);
  gtk_label_set_text(GTK_LABEL(gv->debug_lbl), buf);
  gv->last_debug_refresh_us = now_fc_us;
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->canvas_stack == NULL || !gtk_widget_get_mapped(widget)) {
    gv->tick_id = 0;
    gv->last_tick_us = 0;
    gv->prev_frame_clock_us = 0;
    update_debug_overlay(gv, 0);
    return G_SOURCE_REMOVE;
  }

  gint64 fc_us = (clock != NULL) ? gdk_frame_clock_get_frame_time(clock) : 0;
  record_frame_clock_sample(gv, fc_us);

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

  /* 2b. Smooth hover fisheye spread in 3D/Galaxy when hovering (disabled by Reduce motion). */
  gdouble target_spread = (gv->has_mouse && !gv->dragging && !gv->reduce_motion &&
                           mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL)
                              ? 1.0
                              : 0.0;
  if (gv->reduce_motion) {
    gv->spread_str = 0.0;
  } else if (fabs(gv->spread_str - target_spread) > 0.02) {
    gv->spread_str += (target_spread - gv->spread_str) * 0.28;
    active = TRUE;
  } else {
    gv->spread_str = target_spread;
  }

  /* 2c. Smooth 700ms 3D <-> Galaxy layout transition. */
  if (gv->morphing_3d && gv->pts3d != NULL && gv->pts3d_from != NULL && gv->pts3d_target != NULL) {
    gv->morph_t = gv->reduce_motion ? 1.0 : MIN(gv->morph_t + dt / 0.70, 1.0);
    gdouble e = 1.0 - pow(1.0 - gv->morph_t, 3.0); /* ease-out cubic */
    float fe = (float)e;
    for (guint i = 0; i < gv->pts3d_count; i++) {
      gv->pts3d[i].x = gv->pts3d_from[i].x + (gv->pts3d_target[i].x - gv->pts3d_from[i].x) * fe;
      gv->pts3d[i].y = gv->pts3d_from[i].y + (gv->pts3d_target[i].y - gv->pts3d_from[i].y) * fe;
      gv->pts3d[i].z = gv->pts3d_from[i].z + (gv->pts3d_target[i].z - gv->pts3d_from[i].z) * fe;
    }
    bt_graph_gl_set_layout3d(gv->gl, gv->pts3d, gv->pts3d_count);
    if (gv->morph_t >= 1.0) {
      gv->morphing_3d = FALSE;
      g_clear_pointer(&gv->pts3d_from, g_free);
      g_clear_pointer(&gv->pts3d_target, g_free);
    } else {
      active = TRUE;
    }
  }

  /* 2d. Smooth Organic <-> Kept timeline animation. */
  gdouble target_kt = gv->tl_kept ? 1.0 : 0.0;
  if (gv->reduce_motion) {
    gv->tl_kept_t = target_kt;
  } else if (fabs(gv->tl_kept_t - target_kt) > 0.01) {
    gv->tl_kept_t += (target_kt - gv->tl_kept_t) * 0.20;
    active = TRUE;
  } else {
    gv->tl_kept_t = target_kt;
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

  /* 5. Gentle 3D idle rotation (in Galaxy mode: differential galactic rotation instead of rigid orbit). */
  if (gv->mode == BT_GRAPH_MODE_GALAXY && gv->active_backend == BT_GRAPH_BACKEND_GL &&
      !gv->reduce_motion && !gv->dragging && gv->window_active &&
      gv->graph != NULL && gv->graph->nodes->len > 0) {
    gv->gal_time += dt;
    active = TRUE;
  } else if (needs_idle_orbit(gv) && !gv->animating_3d) {
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
    gv->prev_frame_clock_us = 0;
    update_debug_overlay(gv, fc_us);
    return G_SOURCE_REMOVE;
  }
  if (gv->debug_overlay && (fc_us - gv->last_debug_refresh_us >= 200000 || gv->last_debug_refresh_us == 0))
    update_debug_overlay(gv, fc_us);
  return G_SOURCE_CONTINUE;
}

static void ensure_tick(BtGraphView *gv)
{
  if (gv->canvas_stack == NULL || gv->tick_id != 0 || !gtk_widget_get_mapped(gv->canvas_stack))
    return;
  gv->last_tick_us = g_get_monotonic_time();
  gv->prev_frame_clock_us = 0;
  gv->tick_id = gtk_widget_add_tick_callback(gv->canvas_stack, on_tick, gv, NULL);
  update_debug_overlay(gv, 0);
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
    gv->prev_frame_clock_us = 0;
    update_debug_overlay(gv, 0);
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
  out->show_links = gv->show_links;
  out->focus_isolated = gv->focus_isolated;
  out->mouse_x = gv->last_mouse_x;
  out->mouse_y = gv->last_mouse_y;
  out->spread_str = gv->spread_str;
  out->gal_time = (gv->mode == BT_GRAPH_MODE_GALAXY) ? gv->gal_time : 0.0;
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

static void ensure_timeline_layout(BtGraphView *gv, int w, int h)
{
  if (gv == NULL || gv->graph == NULL || w <= 40 || h <= 60)
    return;
  if (gv->timeline != NULL && !gv->tl_dirty && gv->tl_w == w && gv->tl_h == h)
    return;
  bt_timeline_free(gv->timeline);
  float x0 = 54.0f, x1 = (float)w - 54.0f;
  float y0 = 104.0f, y1 = (float)h - 132.0f;
  gv->timeline = bt_timeline_build(gv->graph, gv->tl_lane_by, x0, x1, y0, y1,
                                   gv->tl_view_min, gv->tl_view_max);
  if (gv->tl_view_min <= 0 || gv->tl_view_max <= gv->tl_view_min) {
    gv->tl_view_min = gv->timeline->min_ts;
    gv->tl_view_max = gv->timeline->max_ts;
  }
  gv->tl_w = w;
  gv->tl_h = h;
  gv->tl_dirty = FALSE;
}

static void draw_timeline_cairo(BtGraphView *gv, cairo_t *cr, int w, int h)
{
  /* Mindcorpus-inspired pure black background. */
  cairo_set_source_rgb(cr, 0.008, 0.010, 0.016);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  if (gv->graph == NULL || gv->graph->nodes->len == 0u)
    return;
  ensure_timeline_layout(gv, w, h);
  const BtTimelineLayout *tl = gv->timeline;
  if (tl == NULL)
    return;

  float x0 = 54.0f, x1 = (float)w - 54.0f;
  float y0 = 104.0f, y1 = (float)h - 132.0f;
  float axis_y = y1 + 18.0f;
  float ledger_y = (float)h - 62.0f;
  float kt = (float)CLAMP(gv->tl_kept_t, 0.0, 1.0);
  gdouble t_span = MAX((gdouble)(gv->tl_view_max - gv->tl_view_min), 60.0);

  PangoLayout *pl = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = pango_font_description_from_string("Inter, Segoe UI, Sans 9");
  pango_font_description_set_size(fd, (gint)(9.0 * PANGO_SCALE));
  pango_layout_set_font_description(pl, fd);

  /* Editorial uppercase header. */
  cairo_set_source_rgba(cr, 0.62, 0.65, 0.72, 0.78);
  pango_layout_set_text(pl, "VAULT, KEPT WITH ITS DATES", -1);
  cairo_move_to(cr, x0, 56.0);
  pango_cairo_show_layout(cr, pl);

  /* Framed Kept container box (fades in during Organic -> Kept animation). */
  if (kt > 0.02f) {
    cairo_set_source_rgba(cr, 0.85, 0.88, 0.96, 0.18 * kt);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x0 + 14.0, y0 + 8.0, (x1 - x0) - 28.0, (y1 - y0) - 16.0);
    cairo_stroke(cr);
  }

  /* Selected/hovered wikilink neighbor arcs. */
  gint foc = (gv->selected_idx >= 0) ? gv->selected_idx : gv->hover_idx;
  if (foc >= 0 && (guint)foc < tl->dot_count && tl->dots[foc].valid && gv->graph->edges != NULL) {
    float fx = tl->dots[foc].org_x + (tl->dots[foc].kept_x - tl->dots[foc].org_x) * kt;
    float fy = tl->dots[foc].org_y + (tl->dots[foc].kept_y - tl->dots[foc].org_y) * kt;
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 0.66, 0.62, 0.98, 0.34);
    for (guint i = 0; i < gv->graph->edges->len; i++) {
      const BtGraphEdge *e = &g_array_index(gv->graph->edges, BtGraphEdge, i);
      if (e->kind == BT_EDGE_CONTAINS) continue;
      gint other = -1;
      if ((gint)e->src_idx == foc) other = (gint)e->dst_idx;
      else if ((gint)e->dst_idx == foc) other = (gint)e->src_idx;
      if (other < 0 || (guint)other >= tl->dot_count || !tl->dots[other].valid) continue;
      float ox = tl->dots[other].org_x + (tl->dots[other].kept_x - tl->dots[other].org_x) * kt;
      float oy = tl->dots[other].org_y + (tl->dots[other].kept_y - tl->dots[other].org_y) * kt;
      float mx = 0.5f * (fx + ox);
      float my = MIN(fy, oy) - 26.0f;
      cairo_move_to(cr, fx, fy);
      cairo_curve_to(cr, mx, my, mx, my, ox, oy);
      cairo_stroke(cr);
    }
  }

  /* Dots in horizontal wavy lanes / Kept grid. */
  gboolean has_brush = (gv->tl_brush_t1 > gv->tl_brush_t0);
  for (guint i = 0; i < tl->dot_count; i++) {
    const BtTimelineDot *d = &tl->dots[i];
    if (!d->valid) continue;
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    float sx = d->org_x + (d->kept_x - d->org_x) * kt;
    float sy = d->org_y + (d->kept_y - d->org_y) * kt;
    if (sx < x0 - 12.0f || sx > x1 + 12.0f) continue;

    gdouble alpha = 0.90;
    if (has_brush && (d->ts < gv->tl_brush_t0 || d->ts > gv->tl_brush_t1))
      alpha = 0.08;
    if (foc >= 0 && (gint)i == foc)
      alpha = 1.0;

    guint32 rgb = node_color(nd);
    set_tok(cr, rgb, alpha);
    cairo_arc(cr, sx, sy, d->radius, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    if ((gint)i == foc) {
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
      cairo_set_line_width(cr, 1.4);
      cairo_arc(cr, sx, sy, d->radius + 3.0, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);

      /* Bright thin vertical connector up to pinned top label bar. */
      cairo_set_source_rgba(cr, 0.95, 0.96, 1.0, 0.80);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, sx, sy - d->radius - 3.0);
      cairo_line_to(cr, sx, 76.0);
      cairo_stroke(cr);

      GDateTime *dt = g_date_time_new_from_unix_utc(d->ts);
      g_autofree gchar *dstr = dt ? g_date_time_format(dt, "%Y-%m-%d") : g_strdup("");
      if (dt) g_date_time_unref(dt);
      g_autofree gchar *pin = g_strdup_printf("%s  ·  %s%s%s",
                                              nd->label ? nd->label : "Untitled",
                                              dstr,
                                              nd->top_folder ? "  ·  " : "",
                                              nd->top_folder ? nd->top_folder : "");
      pango_layout_set_text(pl, pin, -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(pl, &tw, &th);
      gdouble px = CLAMP((gdouble)sx - (gdouble)tw * 0.5, (gdouble)x0, (gdouble)x1 - (gdouble)tw - 10.0);
      draw_rrect(cr, px - 6.0, 56.0, (gdouble)tw + 12.0, (gdouble)th + 8.0, 4.0);
      cairo_set_source_rgba(cr, 0.09, 0.10, 0.14, 0.94);
      cairo_fill_preserve(cr);
      set_tok(cr, rgb, 0.85);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      cairo_set_source_rgba(cr, 0.96, 0.97, 1.0, 1.0);
      cairo_move_to(cr, px, 60.0);
      pango_cairo_show_layout(cr, pl);
    }
  }

  /* Thin horizontal X axis + auto-density date ticks (years -> months -> days). */
  cairo_set_source_rgba(cr, 0.48, 0.52, 0.60, 0.55);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x0, axis_y);
  cairo_line_to(cr, x1, axis_y);
  cairo_stroke(cr);

  /* Brush selection band on X axis. */
  if (has_brush) {
    gdouble bx0 = x0 + ((gdouble)(gv->tl_brush_t0 - gv->tl_view_min) / t_span) * (x1 - x0);
    gdouble bx1 = x0 + ((gdouble)(gv->tl_brush_t1 - gv->tl_view_min) / t_span) * (x1 - x0);
    bx0 = CLAMP(bx0, x0, x1);
    bx1 = CLAMP(bx1, x0, x1);
    cairo_set_source_rgba(cr, 0.55, 0.49, 0.96, 0.22);
    cairo_rectangle(cr, bx0, y0, MAX(bx1 - bx0, 2.0), axis_y - y0);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.66, 0.61, 0.98, 0.90);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, bx0, axis_y);
    cairo_line_to(cr, bx1, axis_y);
    cairo_stroke(cr);
  }

  const gchar *fmt = (t_span > 86400.0 * 400.0) ? "%Y" : ((t_span > 86400.0 * 45.0) ? "%b %Y" : "%b %d");
  int n_ticks = CLAMP((int)((x1 - x0) / 110.0f), 3, 10);
  for (int ti = 0; ti <= n_ticks; ti++) {
    gdouble frac = (gdouble)ti / (gdouble)n_ticks;
    gdouble tx = x0 + frac * (x1 - x0);
    gint64 ts = gv->tl_view_min + (gint64)(frac * t_span);
    cairo_set_source_rgba(cr, 0.48, 0.52, 0.60, 0.65);
    cairo_move_to(cr, tx, axis_y);
    cairo_line_to(cr, tx, axis_y + 5.0);
    cairo_stroke(cr);
    GDateTime *dt = g_date_time_new_from_unix_utc(ts);
    g_autofree gchar *lbl = dt ? g_date_time_format(dt, fmt) : g_strdup("");
    if (dt) g_date_time_unref(dt);
    pango_layout_set_text(pl, lbl, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(pl, &tw, &th);
    cairo_set_source_rgba(cr, 0.65, 0.68, 0.75, 0.85);
    cairo_move_to(cr, tx - (gdouble)tw * 0.5, axis_y + 8.0);
    pango_cairo_show_layout(cr, pl);
  }

  /* Bottom LEDGER strip: hairline + colored squares + tiny uppercase event labels. */
  cairo_set_source_rgba(cr, 0.40, 0.44, 0.52, 0.45);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x0 + 60.0, ledger_y);
  cairo_line_to(cr, x1, ledger_y);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, 0.55, 0.58, 0.66, 0.85);
  pango_layout_set_text(pl, "LEDGER", -1);
  cairo_move_to(cr, x0, ledger_y - 6.0);
  pango_cairo_show_layout(cr, pl);

  for (guint ei = 0; ei < tl->event_count; ei++) {
    const BtLedgerEvent *ev = &tl->events[ei];
    gdouble frac = ((gdouble)(ev->ts - gv->tl_view_min)) / t_span;
    if (frac < 0.0 || frac > 1.0) continue;
    gdouble ex = (x0 + 60.0) + frac * ((x1 - x0) - 60.0);
    set_tok(cr, ev->color_rgb, 0.95);
    cairo_rectangle(cr, ex - 3.5, ledger_y - 3.5, 7.0, 7.0);
    cairo_fill(cr);

    pango_layout_set_text(pl, ev->label, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(pl, &tw, &th);
    gdouble ly = (ei % 2u == 0u) ? (ledger_y - th - 6.0) : (ledger_y + 7.0);
    cairo_set_source_rgba(cr, 0.65, 0.68, 0.75, 0.82);
    cairo_move_to(cr, ex - (gdouble)tw * 0.5, ly);
    pango_cairo_show_layout(cr, pl);
  }

  pango_font_description_free(fd);
  g_object_unref(pl);
}

static void on_cairo_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
  BtGraphView *gv = ud;
  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    draw_timeline_cairo(gv, cr, w, h);
    return;
  }
  BtGraphRenderState st;
  fill_render_state(gv, w, h, gtk_widget_get_scale_factor(GTK_WIDGET(area)), &st);
  if (gv->cairo_renderer != NULL && gv->cairo_renderer->vt->render_cairo != NULL)
    gv->cairo_renderer->vt->render_cairo(gv->cairo_renderer, cr, &st);
}

static guint pick_stack_at(BtGraphView *gv, gdouble sx, gdouble sy, BtPickCand *out, guint out_cap)
{
  if (gv->graph == NULL || gv->graph->nodes->len == 0 || gv->canvas_stack == NULL)
    return 0u;
  int w = gtk_widget_get_width(gv->canvas_stack);
  int h = gtk_widget_get_height(gv->canvas_stack);
  if (w <= 1 || h <= 1)
    return 0u;

  guint n = gv->graph->nodes->len;
  if (n > gv->pick_scratch_cap) {
    g_free(gv->pick_scratch);
    gv->pick_scratch = g_new0(BtScreenNode, n);
    gv->pick_scratch_cap = n;
  }

  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    ensure_timeline_layout(gv, w, h);
    if (gv->timeline == NULL)
      return 0u;
    float kt = (float)CLAMP(gv->tl_kept_t, 0.0, 1.0);
    guint cnt = MIN(n, gv->timeline->dot_count);
    for (guint i = 0; i < cnt; i++) {
      const BtTimelineDot *d = &gv->timeline->dots[i];
      BtScreenNode *sn = &gv->pick_scratch[i];
      sn->node_idx = i;
      sn->visible = d->valid;
      sn->sx = d->org_x + (d->kept_x - d->org_x) * kt;
      sn->sy = d->org_y + (d->kept_y - d->org_y) * kt;
      sn->radius = d->radius;
      sn->depth = 0.0f;
    }
    return bt_graph_pick_screen(gv->pick_scratch, cnt, (float)w, (float)h,
                                (float)sx, (float)sy, out, out_cap);
  }

  gboolean is3d = (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL &&
                   gv->pts3d != NULL && gv->pts3d_count > 0);
  float mvp[16];
  if (is3d) {
    bt_graph_build_orbit_mvp(gv->yaw, gv->pitch, MAX(gv->dist3d, 1.2),
                             (gdouble)w / (gdouble)h, mvp);
  }

  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    BtScreenNode *sn = &gv->pick_scratch[i];
    sn->node_idx = i;
    sn->visible = FALSE;
    if (!nd->visible)
      continue;

    if (is3d) {
      if (i >= gv->pts3d_count)
        continue;
      BtGraphVec3 pt = gv->pts3d[i];
      if (gv->mode == BT_GRAPH_MODE_GALAXY && fabs(gv->gal_time) > 1e-6)
        bt_galaxy_apply_diff_rot(&pt, gv->gal_time, &pt);
      gdouble px = 0.0, py = 0.0, pw = 0.0;
      if (!bt_graph_project_3d(mvp, &pt, (gdouble)w, (gdouble)h, &px, &py, &pw))
        continue;
      float persp = CLAMP(2.4f / MAX((float)pw, 0.35f), 0.40f, 2.2f);
      sn->sx = (float)px;
      sn->sy = (float)py;
      sn->radius = CLAMP((float)nd->radius * persp * 0.85f, 3.0f, 14.0f);
      sn->depth = (float)pw;
      sn->visible = TRUE;
    } else {
      sn->sx = (float)(w * 0.5 + gv->pan_x + nd->x * gv->zoom);
      sn->sy = (float)(h * 0.5 + gv->pan_y + nd->y * gv->zoom);
      sn->radius = (float)bt_graph_screen_core_radius(nd->radius, gv->zoom);
      /* In 2D, put folders slightly in front, then higher degree */
      sn->depth = (nd->kind == BT_NODE_FOLDER) ? -1.0f : (0.0f - (float)nd->degree * 0.0001f);
      sn->visible = TRUE;
    }
  }

  if (is3d && !gv->reduce_motion && gv->spread_str > 0.01) {
    bt_graph_apply_hover_spread(gv->pick_scratch, n, (float)gv->last_mouse_x,
                                (float)gv->last_mouse_y, 40.0f, (float)gv->spread_str);
  }

  return bt_graph_pick_screen(gv->pick_scratch, n, (float)w, (float)h,
                              (float)sx, (float)sy, out, out_cap);
}

static gint pick_node_at(BtGraphView *gv, gdouble sx, gdouble sy)
{
  BtPickCand cands[16];
  guint total = pick_stack_at(gv, sx, sy, cands, 16u);
  if (total == 0u) {
    gv->stack_count = 0u;
    gv->stack_total = 0u;
    gv->stack_idx = 0u;
    return -1;
  }
  gboolean same_spot = (hypot(sx - gv->stack_x, sy - gv->stack_y) <= 6.0 && gv->stack_count > 0u);
  guint cnt = MIN(total, 16u);
  memcpy(gv->stack_cands, cands, cnt * sizeof(BtPickCand));
  gv->stack_count = cnt;
  gv->stack_total = total;
  if (!same_spot || gv->stack_idx >= cnt) {
    gv->stack_idx = 0u;
    gv->stack_x = sx;
    gv->stack_y = sy;
  }
  return (gint)gv->stack_cands[gv->stack_idx].node_idx;
}

/* ---------------------------------------------- pointer & toolbar UI -- */

static void center_on_node(BtGraphView *gv, gint hit)
{
  if (gv == NULL || gv->graph == NULL || hit < 0 || (guint)hit >= gv->graph->nodes->len)
    return;
  const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
  if (mode_is_3d_orbit(gv->mode) && gv->pts3d != NULL && (guint)hit < gv->pts3d_count) {
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

static void apply_kind_visibility(BtGraphView *gv, gboolean upload_gl);
static void update_hover_readout(BtGraphView *gv);

static void toggle_folder_node(BtGraphView *gv, gint idx)
{
  if (gv == NULL || gv->graph == NULL || idx < 0 || (guint)idx >= gv->graph->nodes->len)
    return;
  BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)idx);
  if (nd->kind != BT_NODE_FOLDER)
    return;
  bt_graph_set_folder_collapsed(gv->graph, (guint)idx, !nd->collapsed);
  apply_kind_visibility(gv, TRUE);
  update_hover_readout(gv);
}

static void clear_selection(BtGraphView *gv)
{
  if (gv == NULL)
    return;
  gboolean had = (gv->selected_idx >= 0 || gv->focus_isolated);
  gv->selected_idx = -1;
  gv->focus_isolated = FALSE;
  gv->focus_hops = 1u;
  if (!had)
    return;
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
  (void)c; (void)keycode;
  BtGraphView *gv = ud;
  if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
      (keyval == GDK_KEY_F || keyval == GDK_KEY_f)) {
    bt_graphview_toggle_debug_overlay(gv);
    return TRUE;
  }
  if (keyval == GDK_KEY_Escape && (gv->selected_idx >= 0 || gv->focus_isolated)) {
    clear_selection(gv);
    return TRUE;
  }
  if (keyval == GDK_KEY_Tab && gv->stack_count >= 2u) {
    gv->stack_idx = (gv->stack_idx + 1u) % gv->stack_count;
    gint hit = (gint)gv->stack_cands[gv->stack_idx].node_idx;
    gv->hover_idx = hit;
    gv->selected_idx = hit;
    refresh_neighbor_mask(gv);
    update_hover_readout(gv);
    queue_canvas_draw(gv);
    return TRUE;
  }
  if (keyval == GDK_KEY_space && gv->selected_idx >= 0 && gv->graph != NULL &&
      (guint)gv->selected_idx < gv->graph->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)gv->selected_idx);
    if (nd->kind == BT_NODE_FOLDER) {
      toggle_folder_node(gv, gv->selected_idx);
      return TRUE;
    }
  }
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && gv->selected_idx >= 0 &&
      gv->graph != NULL && (guint)gv->selected_idx < gv->graph->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)gv->selected_idx);
    if (nd->kind == BT_NODE_FOLDER) {
      toggle_folder_node(gv, gv->selected_idx);
      return TRUE;
    }
    if (nd->relpath != NULL && !nd->broken && gv->on_open != NULL) {
      gv->on_open(nd->relpath, nd->kind == BT_NODE_GHOST, gv->user_data);
      return TRUE;
    }
  }
  if ((keyval == GDK_KEY_f || keyval == GDK_KEY_F) && (state & GDK_CONTROL_MASK) == 0 &&
      gv->selected_idx >= 0) {
    center_on_node(gv, gv->selected_idx);
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

  /* Single click at same spot with overlapping stack cycles front-to-back. */
  gboolean was_same_spot = (n_press == 1 && gv->stack_count >= 2u &&
                            hypot(x - gv->stack_x, y - gv->stack_y) <= 6.0 &&
                            gv->selected_idx == (gint)gv->stack_cands[gv->stack_idx].node_idx);
  if (was_same_spot)
    gv->stack_idx = (gv->stack_idx + 1u) % gv->stack_count;

  gint hit = pick_node_at(gv, x, y);
  if (hit >= 0) {
    gv->selected_idx = hit;
    gv->hover_idx = hit;
    gv->pulse_phase = 0.0;
    refresh_neighbor_mask(gv);
    sync_toolbar_buttons(gv);
    update_hover_readout(gv);
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    if (n_press == 1 && nd->relpath != NULL && !nd->broken && gv->on_select != NULL)
      gv->on_select(nd->relpath, gv->select_ud);
    if (n_press == 2) {
      if (nd->kind == BT_NODE_FOLDER) {
        toggle_folder_node(gv, hit);
      } else if (nd->relpath != NULL && !nd->broken && gv->on_open != NULL) {
        gv->on_open(nd->relpath, nd->kind == BT_NODE_GHOST, gv->user_data);
      } else {
        center_on_node(gv, hit);
      }
      return;
    }
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

  if (gv->mode == BT_GRAPH_MODE_TIMELINE && gv->canvas_stack != NULL) {
    int w = gtk_widget_get_width(gv->canvas_stack);
    int h = gtk_widget_get_height(gv->canvas_stack);
    float x0 = 54.0f, x1 = (float)w - 54.0f;
    float axis_y = (float)h - 114.0f;
    float ledger_y = (float)h - 62.0f;
    gdouble t_span = MAX((gdouble)(gv->tl_view_max - gv->tl_view_min), 60.0);

    /* Click on bottom LEDGER event -> brush-select +/- 5% time window around event. */
    if (gv->timeline != NULL && fabs(y - ledger_y) <= 18.0) {
      for (guint ei = 0; ei < gv->timeline->event_count; ei++) {
        const BtLedgerEvent *ev = &gv->timeline->events[ei];
        gdouble frac = ((gdouble)(ev->ts - gv->tl_view_min)) / t_span;
        gdouble ex = (x0 + 60.0) + frac * ((x1 - x0) - 60.0);
        if (fabs(x - ex) <= 18.0) {
          gint64 pad = MAX((gint64)(t_span * 0.06), 43200);
          gv->tl_brush_t0 = ev->ts - pad;
          gv->tl_brush_t1 = ev->ts + pad;
          queue_canvas_draw(gv);
          return;
        }
      }
    }

    /* Drag on X axis -> brush-filter time range. */
    if (fabs(y - axis_y) <= 22.0 && x >= x0 && x <= x1) {
      gv->tl_brushing = TRUE;
      gv->tl_brush_x0 = x;
      gv->tl_brush_x1 = x;
      gv->tl_brush_t0 = 0;
      gv->tl_brush_t1 = 0;
      queue_canvas_draw(gv);
      return;
    }

    /* Clear brush on clean background click; start timeline pan. */
    gv->tl_brush_t0 = 0;
    gv->tl_brush_t1 = 0;
    gv->tl_pan_start_min = gv->tl_view_min;
    gv->tl_pan_start_max = gv->tl_view_max;
  }

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
  if (mode_is_3d_orbit(gv->mode))
    gv->user_interacted_3d = TRUE;
}

static void on_click_released(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)g; (void)n_press; (void)x; (void)y;
  BtGraphView *gv = ud;
  gboolean was_clean_bg_click = gv->dragging && !gv->drag_moved;
  gv->dragging = FALSE;
  gv->drag_moved = FALSE;
  gv->tl_brushing = FALSE;
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

static void update_hover_readout(BtGraphView *gv);

static void on_motion(GtkEventControllerMotion *c, gdouble x, gdouble y, gpointer ud)
{
  (void)c;
  BtGraphView *gv = ud;
  gv->has_mouse = TRUE;
  gv->last_mouse_x = x;
  gv->last_mouse_y = y;
  if (gv->tl_brushing && gv->mode == BT_GRAPH_MODE_TIMELINE && gv->canvas_stack != NULL) {
    int w = gtk_widget_get_width(gv->canvas_stack);
    gdouble x0 = 54.0, x1 = MAX((gdouble)w - 54.0, x0 + 40.0);
    gv->tl_brush_x1 = CLAMP(x, x0, x1);
    gdouble a = MIN(gv->tl_brush_x0, gv->tl_brush_x1);
    gdouble b = MAX(gv->tl_brush_x0, gv->tl_brush_x1);
    gdouble t_span = MAX((gdouble)(gv->tl_view_max - gv->tl_view_min), 60.0);
    gv->tl_brush_t0 = gv->tl_view_min + (gint64)(((a - x0) / (x1 - x0)) * t_span);
    gv->tl_brush_t1 = gv->tl_view_min + (gint64)(((b - x0) / (x1 - x0)) * t_span);
    queue_canvas_draw(gv);
    return;
  }
  if (gv->dragging) {
    if (hypot(x - gv->drag_start_x, y - gv->drag_start_y) > 4.0)
      gv->drag_moved = TRUE;
    if (gv->mode == BT_GRAPH_MODE_TIMELINE && gv->canvas_stack != NULL) {
      int w = gtk_widget_get_width(gv->canvas_stack);
      gdouble span_px = MAX((gdouble)w - 108.0, 40.0);
      gdouble t_span = MAX((gdouble)(gv->tl_pan_start_max - gv->tl_pan_start_min), 60.0);
      gint64 dt = (gint64)(-((x - gv->drag_start_x) / span_px) * t_span);
      gv->tl_view_min = gv->tl_pan_start_min + dt;
      gv->tl_view_max = gv->tl_pan_start_max + dt;
      gv->tl_dirty = TRUE;
    } else if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL) {
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
      update_hover_readout(gv);
      if (gv->reduce_motion) {
        gv->hover_fade = (hit >= 0 || gv->selected_idx >= 0 || gv->focus_idx >= 0) ? 1.0 : 0.0;
        queue_canvas_draw(gv);
      } else {
        ensure_tick(gv);
      }
    } else if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL &&
               !gv->reduce_motion) {
      ensure_tick(gv);
      queue_canvas_draw(gv);
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
    update_hover_readout(gv);
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
  if (gv->mode == BT_GRAPH_MODE_TIMELINE && gv->canvas_stack != NULL) {
    int w = gtk_widget_get_width(gv->canvas_stack);
    gdouble x0 = 54.0, x1 = MAX((gdouble)w - 54.0, x0 + 40.0);
    gdouble frac = CLAMP((gv->last_mouse_x - x0) / (x1 - x0), 0.0, 1.0);
    gdouble t_span = MAX((gdouble)(gv->tl_view_max - gv->tl_view_min), 3600.0);
    gint64 anchor = gv->tl_view_min + (gint64)(frac * t_span);
    gdouble factor = (dy < 0.0) ? 0.84 : 1.19;
    gdouble new_span = CLAMP(t_span * factor, 3600.0, 86400.0 * 365.0 * 60.0);
    gv->tl_view_min = anchor - (gint64)(frac * new_span);
    gv->tl_view_max = gv->tl_view_min + (gint64)new_span;
    gv->tl_dirty = TRUE;
    queue_canvas_draw(gv);
    return TRUE;
  }
  if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL) {
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
  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    gdouble t_span = MAX((gdouble)(gv->tl_view_max - gv->tl_view_min), 3600.0);
    gint64 mid = gv->tl_view_min + (gint64)(t_span * 0.5);
    gdouble ns = CLAMP(t_span * 0.78, 3600.0, 86400.0 * 365.0 * 60.0);
    gv->tl_view_min = mid - (gint64)(ns * 0.5);
    gv->tl_view_max = gv->tl_view_min + (gint64)ns;
    gv->tl_dirty = TRUE;
    queue_canvas_draw(gv);
    return;
  }
  if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL) {
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
  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    gdouble t_span = MAX((gdouble)(gv->tl_view_max - gv->tl_view_min), 3600.0);
    gint64 mid = gv->tl_view_min + (gint64)(t_span * 0.5);
    gdouble ns = CLAMP(t_span * 1.28, 3600.0, 86400.0 * 365.0 * 60.0);
    gv->tl_view_min = mid - (gint64)(ns * 0.5);
    gv->tl_view_max = gv->tl_view_min + (gint64)ns;
    gv->tl_dirty = TRUE;
    queue_canvas_draw(gv);
    return;
  }
  if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL) {
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
  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    gv->tl_view_min = 0;
    gv->tl_view_max = 0;
    gv->tl_brush_t0 = 0;
    gv->tl_brush_t1 = 0;
    gv->tl_dirty = TRUE;
    queue_canvas_draw(gv);
    return;
  }
  if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->target_yaw = 0.35;
    gv->target_pitch = (gv->mode == BT_GRAPH_MODE_GALAXY) ? 0.58 : 0.20;
    gv->target_dist3d = (gv->mode == BT_GRAPH_MODE_GALAXY) ? 2.65 : 2.8;
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
  if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    gv->tl_view_min = 0;
    gv->tl_view_max = 0;
    gv->tl_brush_t0 = 0;
    gv->tl_brush_t1 = 0;
    gv->tl_dirty = TRUE;
    queue_canvas_draw(gv);
    return;
  }
  if (mode_is_3d_orbit(gv->mode) && gv->active_backend == BT_GRAPH_BACKEND_GL) {
    gv->target_yaw = 0.35;
    gv->target_pitch = (gv->mode == BT_GRAPH_MODE_GALAXY) ? 0.58 : 0.20;
    gv->target_dist3d = (gv->mode == BT_GRAPH_MODE_GALAXY) ? 2.65 : 2.8;
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
  gboolean is3d = gl_ok && (gv->mode == BT_GRAPH_MODE_3D);
  gboolean is_gal = gl_ok && (gv->mode == BT_GRAPH_MODE_GALAXY);
  gboolean is_tl = (gv->mode == BT_GRAPH_MODE_TIMELINE);
  if (gv->btn_2d != NULL) {
    if (!is3d && !is_gal && !is_tl)
      gtk_widget_add_css_class(gv->btn_2d, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_2d, "bt-pill-active");
  }
  if (gv->btn_3d != NULL) {
    gtk_widget_set_sensitive(gv->btn_3d, gl_ok);
    gtk_widget_set_tooltip_text(
        gv->btn_3d, gl_ok ? "3D cortex orbit view (drag to orbit, scroll to dolly)"
                          : "3D orbit view requires OpenGL (running Cairo fallback)");
    if (is3d)
      gtk_widget_add_css_class(gv->btn_3d, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_3d, "bt-pill-active");
  }
  if (gv->btn_galaxy != NULL) {
    gtk_widget_set_sensitive(gv->btn_galaxy, gl_ok);
    gtk_widget_set_tooltip_text(
        gv->btn_galaxy, gl_ok ? "Galaxy spiral disk view (folder star clusters, orbitable 3D)"
                              : "Galaxy view requires OpenGL (running Cairo fallback)");
    if (is_gal)
      gtk_widget_add_css_class(gv->btn_galaxy, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_galaxy, "bt-pill-active");
  }
  if (gv->btn_timeline != NULL) {
    if (is_tl)
      gtk_widget_add_css_class(gv->btn_timeline, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_timeline, "bt-pill-active");
  }
  if (gv->tl_controls_box != NULL)
    gtk_widget_set_visible(gv->tl_controls_box, is_tl);
  if (gv->mode_badge_lbl != NULL) {
    const gchar *badge = is_gal ? "GALAXY VIEW"
                         : is_tl ? "TIMELINE VIEW"
                         : is3d  ? "3D VIEW"
                                 : "2D VIEW";
    gtk_label_set_text(GTK_LABEL(gv->mode_badge_lbl), badge);
  }
  if (gv->btn_tl_organic != NULL) {
    if (!gv->tl_kept) gtk_widget_add_css_class(gv->btn_tl_organic, "bt-pill-active");
    else gtk_widget_remove_css_class(gv->btn_tl_organic, "bt-pill-active");
  }
  if (gv->btn_tl_kept != NULL) {
    if (gv->tl_kept) gtk_widget_add_css_class(gv->btn_tl_kept, "bt-pill-active");
    else gtk_widget_remove_css_class(gv->btn_tl_kept, "bt-pill-active");
  }
  if (gv->btn_tl_lane_folder != NULL) {
    if (gv->tl_lane_by == BT_TL_LANE_FOLDER)
      gtk_widget_add_css_class(gv->btn_tl_lane_folder, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_tl_lane_folder, "bt-pill-active");
  }
  if (gv->btn_tl_lane_type != NULL) {
    if (gv->tl_lane_by == BT_TL_LANE_TYPE)
      gtk_widget_add_css_class(gv->btn_tl_lane_type, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_tl_lane_type, "bt-pill-active");
  }
  gboolean hi = (gv->quality == BT_GRAPH_QUALITY_HIGH);
  if (gv->btn_q_low != NULL) {
    if (!hi)
      gtk_widget_add_css_class(gv->btn_q_low, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_q_low, "bt-pill-active");
  }
  if (gv->btn_q_high != NULL) {
    if (hi)
      gtk_widget_add_css_class(gv->btn_q_high, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_q_high, "bt-pill-active");
  }
  if (gv->btn_pulse != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_pulse), gv->pulse_enabled);
    gtk_widget_set_visible(gv->btn_pulse, gl_ok && gv->selected_idx >= 0);
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
  if (gv->btn_links != NULL) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gv->btn_links), gv->show_links);
  }
  if (gv->btn_col_folder != NULL) {
    if (gv->color_by_folder)
      gtk_widget_add_css_class(gv->btn_col_folder, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_col_folder, "bt-pill-active");
  }
  if (gv->btn_col_type != NULL) {
    if (!gv->color_by_folder)
      gtk_widget_add_css_class(gv->btn_col_type, "bt-pill-active");
    else
      gtk_widget_remove_css_class(gv->btn_col_type, "bt-pill-active");
  }
}

static const gchar *node_kind_label(BtNodeKind k)
{
  switch (k) {
  case BT_NODE_HUMAN_NOTE:  return "Note";
  case BT_NODE_AI_NOTE:     return "AI note";
  case BT_NODE_TAG:         return "Tag";
  case BT_NODE_FOLDER:      return "Folder";
  case BT_NODE_SESSION:     return "Session";
  case BT_NODE_IMPORT_ROOT: return "Import root";
  case BT_NODE_GHOST:       return "Ghost";
  default:                  return "Node";
  }
}

static void update_hover_readout(BtGraphView *gv)
{
  if (gv == NULL || gv->hover_lbl == NULL)
    return;
  if (gv->graph == NULL || gv->hover_idx < 0 || (guint)gv->hover_idx >= gv->graph->nodes->len) {
    gtk_widget_set_visible(gv->hover_lbl, FALSE);
    return;
  }
  const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)gv->hover_idx);
  if (!nd->visible) {
    gtk_widget_set_visible(gv->hover_lbl, FALSE);
    return;
  }
  const gchar *title = (nd->label != NULL && nd->label[0] != '\0') ? nd->label : nd->id;
  g_autofree gchar *stack_chip = NULL;
  if (gv->stack_total >= 2u) {
    stack_chip = g_strdup_printf("  [+%u · %u/%u Tab/click]",
                                 gv->stack_total - 1u, gv->stack_idx + 1u, gv->stack_total);
  }
  gchar *txt = NULL;
  if (nd->kind == BT_NODE_FOLDER) {
    txt = g_strdup_printf("%s · %u %s · Folder%s%s",
                          title ? title : "Folder",
                          nd->child_count,
                          nd->child_count == 1u ? "note" : "notes",
                          nd->collapsed ? " (collapsed)" : "",
                          stack_chip ? stack_chip : "");
  } else {
    txt = g_strdup_printf("%s · %u %s · %s%s",
                          title ? title : "Untitled",
                          nd->degree,
                          nd->degree == 1u ? "link" : "links",
                          node_kind_label(nd->kind),
                          stack_chip ? stack_chip : "");
  }
  gtk_label_set_text(GTK_LABEL(gv->hover_lbl), txt);
  gtk_widget_set_visible(gv->hover_lbl, TRUE);
  g_free(txt);
}

static void apply_folder_colors_and_refresh(BtGraphView *gv, gboolean upload_gl);

static void on_legend_folder_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                                   gpointer ud)
{
  (void)x; (void)y;
  if (n_press != 1)
    return;
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  BtGraphView *gv = ud;
  const gchar *fname = g_object_get_data(G_OBJECT(row), "bt-folder-name");
  if (gv == NULL || gv->graph == NULL || fname == NULL)
    return;

  GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
  if ((mods & GDK_CONTROL_MASK) != 0) {
    /* Ctrl+click = toggle hide that top-level folder */
    for (guint i = 0; i < gv->graph->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      if (nd->kind == BT_NODE_FOLDER && nd->parent_idx < 0 &&
          g_strcmp0(nd->top_folder, fname) == 0) {
        bt_graph_set_folder_hidden(gv->graph, i, !nd->hidden_by_user);
        break;
      }
    }
  } else {
    /* Click = isolate that folder (click again to clear isolation) */
    if (g_strcmp0(gv->isolated_folder, fname) == 0) {
      g_clear_pointer(&gv->isolated_folder, g_free);
    } else {
      g_free(gv->isolated_folder);
      gv->isolated_folder = g_strdup(fname);
    }
  }
  apply_kind_visibility(gv, TRUE);
  queue_canvas_draw(gv);
}

static void update_legend(BtGraphView *gv)
{
  if (gv == NULL)
    return;

  if (gv->legend_folders_box != NULL) {
    GtkWidget *child = gtk_widget_get_first_child(gv->legend_folders_box);
    while (child != NULL) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(gv->legend_folders_box), child);
      child = next;
    }

    if (gv->graph != NULL && gv->graph->nodes != NULL) {
      GPtrArray *top_folders = g_ptr_array_new();
      for (guint i = 0; i < gv->graph->nodes->len; i++) {
        BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
        if (nd->kind == BT_NODE_FOLDER && nd->parent_idx < 0 && nd->top_folder != NULL)
          g_ptr_array_add(top_folders, nd);
      }
      for (guint i = 0; i < top_folders->len; i++) {
        const BtGraphNode *fn = g_ptr_array_index(top_folders, i);
        guint32 rgb = fn->color_rgb ? fn->color_rgb : bt_folder_colors_get(gv->folder_colors, fn->top_folder);
        if (rgb == 0u) rgb = BT_GALAXY_ROOT_COLOR;
        gboolean is_iso = (gv->isolated_folder != NULL &&
                           g_strcmp0(gv->isolated_folder, fn->top_folder) == 0);
        g_autofree gchar *esc = g_markup_escape_text(fn->label ? fn->label : fn->top_folder, -1);
        g_autofree gchar *mk = g_strdup_printf(
            "<span foreground='#%06X'>●</span> <span foreground='%s'>%s</span>"
            " <span foreground='#666D79'>(%u)%s%s</span>",
            rgb & 0xFFFFFFu,
            fn->hidden_by_user ? "#666D79" : (is_iso ? "#FFFFFF" : "#D6DAE2"),
            esc, fn->child_count,
            fn->hidden_by_user ? " hidden" : "",
            is_iso ? " ★" : "");
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), mk);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
        gtk_widget_set_tooltip_text(lbl, "Click: isolate folder · Ctrl+click: hide/show folder");
        g_object_set_data_full(G_OBJECT(lbl), "bt-folder-name", g_strdup(fn->top_folder), g_free);
        GtkGesture *clk = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clk), GDK_BUTTON_PRIMARY);
        g_signal_connect(clk, "pressed", G_CALLBACK(on_legend_folder_click), gv);
        gtk_widget_add_controller(lbl, GTK_EVENT_CONTROLLER(clk));
        gtk_box_append(GTK_BOX(gv->legend_folders_box), lbl);
      }
      g_ptr_array_unref(top_folders);
    }
  }

  if (gv->legend_lbl == NULL)
    return;
  gchar *markup = g_strdup_printf(
      "<span foreground='#9AA1AD'>Type:</span> "
      "● Note  ○ AI  "
      "<span foreground='%s'>• Tag%s</span>  "
      "<span foreground='%s'>◌ Ghost%s</span>  "
      "■ Session  ■ Import  ◎ Folder",
      gv->show_tags ? "#FBBF24" : "#666D79",
      gv->show_tags ? "" : " (off)",
      gv->show_ghosts ? "#9AA1AD" : "#666D79",
      gv->show_ghosts ? "" : " (off)");
  gtk_label_set_markup(GTK_LABEL(gv->legend_lbl), markup);
  g_free(markup);
}

static void apply_folder_colors_and_refresh(BtGraphView *gv, gboolean upload_gl)
{
  if (gv == NULL || gv->graph == NULL)
    return;
  if (gv->folder_colors == NULL)
    gv->folder_colors = bt_folder_colors_load(gv->vault_root_path);
  gboolean added = bt_folder_colors_apply(gv->folder_colors, gv->graph,
                                          gv->color_by_folder, 0u);
  if (added && gv->folder_colors->json_path != NULL)
    bt_folder_colors_save(gv->folder_colors, NULL);
  if (upload_gl)
    bt_graph_gl_mark_graph_dirty(gv->gl);
  update_legend(gv);
}

static void apply_kind_visibility(BtGraphView *gv, gboolean upload_gl)
{
  if (gv == NULL || gv->graph == NULL)
    return;
  bt_graph_recompute_visibility(gv->graph, gv->show_tags, gv->show_ghosts);
  if (gv->isolated_folder != NULL) {
    for (guint i = 0; i < gv->graph->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
      if (nd->visible && g_strcmp0(nd->top_folder, gv->isolated_folder) != 0)
        nd->visible = FALSE;
    }
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
  update_hover_readout(gv);
  update_legend(gv);
}

static void notify_prefs_changed(BtGraphView *gv)
{
  if (gv != NULL && gv->on_prefs != NULL)
    gv->on_prefs(gv->show_tags, gv->show_ghosts, gv->prefs_ud);
}

static void on_btn_2d_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_graphview_set_mode(ud, BT_GRAPH_MODE_2D);
  notify_prefs_changed(ud);
}

static void on_btn_3d_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_graphview_set_mode(ud, BT_GRAPH_MODE_3D);
  notify_prefs_changed(ud);
}

static void on_btn_galaxy_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_graphview_set_mode(ud, BT_GRAPH_MODE_GALAXY);
  notify_prefs_changed(ud);
}

static void on_btn_timeline_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_graphview_set_mode(ud, BT_GRAPH_MODE_TIMELINE);
  notify_prefs_changed(ud);
}

static void on_btn_tl_organic_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->tl_kept = FALSE;
  sync_toolbar_buttons(gv);
  if (gv->reduce_motion) {
    gv->tl_kept_t = 0.0;
    queue_canvas_draw(gv);
  } else {
    ensure_tick(gv);
  }
}

static void on_btn_tl_kept_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->tl_kept = TRUE;
  sync_toolbar_buttons(gv);
  if (gv->reduce_motion) {
    gv->tl_kept_t = 1.0;
    queue_canvas_draw(gv);
  } else {
    ensure_tick(gv);
  }
}

static void on_btn_tl_lane_folder_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->tl_lane_by = BT_TL_LANE_FOLDER;
  gv->tl_dirty = TRUE;
  sync_toolbar_buttons(gv);
  queue_canvas_draw(gv);
}

static void on_btn_tl_lane_type_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gv->tl_lane_by = BT_TL_LANE_TYPE;
  gv->tl_dirty = TRUE;
  sync_toolbar_buttons(gv);
  queue_canvas_draw(gv);
}

static void on_btn_q_low_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_graphview_set_quality(ud, BT_GRAPH_QUALITY_LOW);
}

static void on_btn_q_high_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  bt_graphview_set_quality(ud, BT_GRAPH_QUALITY_HIGH);
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

static void on_btn_links_toggled(GtkToggleButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  gboolean en = gtk_toggle_button_get_active(b);
  if (en != gv->show_links)
    bt_graphview_set_show_links(gv, en);
}

static void on_btn_col_folder_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (!gv->color_by_folder) {
    gv->color_by_folder = TRUE;
    apply_folder_colors_and_refresh(gv, TRUE);
    sync_toolbar_buttons(gv);
    queue_canvas_draw(gv);
  }
}

static void on_btn_col_type_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  if (gv->color_by_folder) {
    gv->color_by_folder = FALSE;
    apply_folder_colors_and_refresh(gv, TRUE);
    sync_toolbar_buttons(gv);
    queue_canvas_draw(gv);
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

static void on_gctx_toggle_folder(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  dismiss_ctx_popover(gv);
  toggle_folder_node(gv, hit);
}

static void on_gctx_reveal_sidebar(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gchar *rel = g_strdup(gv->ctx_relpath);
  dismiss_ctx_popover(gv);
  if (rel != NULL && gv->on_select != NULL)
    gv->on_select(rel, gv->select_ud);
  g_free(rel);
}

static void on_gctx_focus_1hop(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  dismiss_ctx_popover(gv);
  if (hit >= 0) {
    gv->selected_idx = hit;
    gv->focus_isolated = TRUE;
    gv->focus_hops = 1u;
    refresh_neighbor_mask(gv);
    gv->hover_fade = 1.0;
    queue_canvas_draw(gv);
  }
}

static void on_gctx_focus_2hop(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  dismiss_ctx_popover(gv);
  if (hit >= 0) {
    gv->selected_idx = hit;
    gv->focus_isolated = TRUE;
    gv->focus_hops = 2u;
    refresh_neighbor_mask(gv);
    gv->hover_fade = 1.0;
    queue_canvas_draw(gv);
  }
}

static void on_gctx_copy_wikilink(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  g_autofree gchar *link = NULL;
  if (gv->graph != NULL && hit >= 0 && (guint)hit < gv->graph->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    const gchar *name = (nd->label != NULL && nd->label[0] != '\0') ? nd->label : nd->relpath;
    if (name != NULL)
      link = g_strdup_printf("[[%s]]", name);
  }
  dismiss_ctx_popover(gv);
  if (link != NULL && gv->canvas_stack != NULL) {
    GdkClipboard *cb = gtk_widget_get_clipboard(gv->canvas_stack);
    if (cb != NULL)
      gdk_clipboard_set_text(cb, link);
    emit_notice(gv, "Copied wikilink to clipboard");
  }
}

static void on_gctx_hide_folder(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  dismiss_ctx_popover(gv);
  if (gv->graph != NULL && hit >= 0 && (guint)hit < gv->graph->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    gint fidx = (nd->kind == BT_NODE_FOLDER) ? hit : nd->parent_idx;
    if (fidx >= 0) {
      bt_graph_set_folder_hidden(gv->graph, (guint)fidx, TRUE);
      apply_kind_visibility(gv, TRUE);
      queue_canvas_draw(gv);
    }
  }
}

static void on_gctx_show_all_folders(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  dismiss_ctx_popover(gv);
  if (gv->graph == NULL)
    return;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    nd->hidden_by_user = FALSE;
  }
  apply_kind_visibility(gv, TRUE);
  queue_canvas_draw(gv);
}

static void on_color_dialog_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  BtGraphView *gv = ud;
  GdkRGBA *rgba = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(src), res, NULL);
  if (rgba == NULL)
    return;
  guint ri = (guint)CLAMP(llround(rgba->red * 255.0), 0, 255);
  guint gi = (guint)CLAMP(llround(rgba->green * 255.0), 0, 255);
  guint bi = (guint)CLAMP(llround(rgba->blue * 255.0), 0, 255);
  gdk_rgba_free(rgba);
  guint32 rgb = (ri << 16) | (gi << 8) | bi;
  if (gv->color_target_folder != NULL) {
    if (gv->folder_colors == NULL)
      gv->folder_colors = bt_folder_colors_load(gv->vault_root_path);
    bt_folder_colors_set(gv->folder_colors, gv->color_target_folder, rgb);
    bt_folder_colors_save(gv->folder_colors, NULL);
    gv->color_by_folder = TRUE;
    apply_folder_colors_and_refresh(gv, TRUE);
    sync_toolbar_buttons(gv);
    queue_canvas_draw(gv);
  }
}

static void on_swatch_clicked(GtkButton *b, gpointer ud)
{
  BtGraphView *gv = ud;
  guint32 rgb = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "bt-swatch-rgb")) & 0xFFFFFFu;
  g_autofree gchar *folder = g_strdup(gv->color_target_folder);
  dismiss_ctx_popover(gv);
  if (folder != NULL) {
    if (gv->folder_colors == NULL)
      gv->folder_colors = bt_folder_colors_load(gv->vault_root_path);
    bt_folder_colors_set(gv->folder_colors, folder, rgb);
    bt_folder_colors_save(gv->folder_colors, NULL);
    gv->color_by_folder = TRUE;
    apply_folder_colors_and_refresh(gv, TRUE);
    sync_toolbar_buttons(gv);
    queue_canvas_draw(gv);
  }
}

static void on_custom_color_clicked(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  dismiss_ctx_popover(gv);
  if (gv->color_target_folder == NULL)
    return;
  GtkColorDialog *dlg = gtk_color_dialog_new();
  gtk_color_dialog_set_title(dlg, "Choose Folder Color");
  gtk_color_dialog_set_with_alpha(dlg, FALSE);
  guint32 cur = bt_folder_colors_get(gv->folder_colors, gv->color_target_folder);
  GdkRGBA init = {
    (float)((cur >> 16) & 0xFFu) / 255.0f,
    (float)((cur >> 8) & 0xFFu) / 255.0f,
    (float)(cur & 0xFFu) / 255.0f,
    1.0f
  };
  gtk_color_dialog_choose_rgba(dlg, gv->toplevel, cur ? &init : NULL, NULL,
                               on_color_dialog_done, gv);
  g_object_unref(dlg);
}

static GtkWidget *make_gctx_btn(const gchar *label, const gchar *accel, GCallback cb,
                                BtGraphView *gv);
static GtkWidget *make_gctx_sep(void);

static void on_gctx_folder_color(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  gint hit = gv->ctx_hit_idx;
  g_autofree gchar *top = NULL;
  if (gv->graph != NULL && hit >= 0 && (guint)hit < gv->graph->nodes->len) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, (guint)hit);
    if (nd->top_folder != NULL)
      top = g_strdup(nd->top_folder);
  }
  dismiss_ctx_popover(gv);
  if (top == NULL || gv->canvas_stack == NULL)
    return;

  g_free(gv->color_target_folder);
  gv->color_target_folder = g_strdup(top);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(box, "bt-ctx-menu");
  GtkWidget *hdr = gtk_label_new(top);
  gtk_widget_add_css_class(hdr, "bt-graph-toolbar-lbl");
  gtk_box_append(GTK_BOX(box), hdr);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
  guint scnt = 0;
  const BtGalaxySwatch *sw = bt_galaxy_palette(&scnt);
  for (guint i = 0; i < scnt; i++) {
    GtkWidget *sb = gtk_button_new();
    gtk_widget_add_css_class(sb, "bt-graph-pill");
    gtk_widget_set_tooltip_text(sb, sw[i].name);
    g_autofree gchar *mk = g_strdup_printf("<span foreground='#%06X'>●</span>", sw[i].rgb);
    GtkWidget *dl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(dl), mk);
    gtk_button_set_child(GTK_BUTTON(sb), dl);
    g_object_set_data(G_OBJECT(sb), "bt-swatch-rgb", GUINT_TO_POINTER(sw[i].rgb));
    g_signal_connect(sb, "clicked", G_CALLBACK(on_swatch_clicked), gv);
    gtk_grid_attach(GTK_GRID(grid), sb, (int)(i % 4u), (int)(i / 4u), 1, 1);
  }
  gtk_box_append(GTK_BOX(box), grid);
  gtk_box_append(GTK_BOX(box), make_gctx_sep());
  gtk_box_append(GTK_BOX(box),
                 make_gctx_btn("Custom Color…", NULL, G_CALLBACK(on_custom_color_clicked), gv));

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_add_css_class(pop, "bt-ctx-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
  gtk_popover_set_child(GTK_POPOVER(pop), box);
  gtk_widget_set_parent(pop, gv->canvas_stack);
  GdkRectangle rect = {(int)gv->last_mouse_x, (int)gv->last_mouse_y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  g_signal_connect(pop, "closed", G_CALLBACK(on_ctx_closed), gv);
  gv->ctx_popover = pop;
  gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_gctx_toggle_mode(GtkButton *b, gpointer ud)
{
  (void)b;
  BtGraphView *gv = ud;
  dismiss_ctx_popover(gv);
  bt_graphview_set_mode(gv, gv->mode == BT_GRAPH_MODE_3D ? BT_GRAPH_MODE_2D : BT_GRAPH_MODE_3D);
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

    gboolean is_folder = (nd->kind == BT_NODE_FOLDER);
    gboolean has_note = (!is_folder && nd->relpath != NULL && !nd->broken);
    gboolean is_real_note = has_note && (nd->kind != BT_NODE_GHOST);

    if (is_folder) {
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn(nd->collapsed ? "Expand Folder" : "Collapse Folder", "Space",
                                   G_CALLBACK(on_gctx_toggle_folder), gv));
    } else if (has_note && gv->on_open != NULL) {
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn(gv->ctx_is_ghost ? "Create Note" : "Open Note", "Enter",
                                   G_CALLBACK(on_gctx_open), gv));
    }
    if ((is_real_note || is_folder) && gv->on_select != NULL)
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn("Reveal in Sidebar", NULL,
                                   G_CALLBACK(on_gctx_reveal_sidebar), gv));
    gtk_box_append(GTK_BOX(box), make_gctx_sep());
    gtk_box_append(GTK_BOX(box),
                   make_gctx_btn("Focus Neighbors (1 hop)", NULL,
                                 G_CALLBACK(on_gctx_focus_1hop), gv));
    gtk_box_append(GTK_BOX(box),
                   make_gctx_btn("Focus Neighbors (2 hops)", NULL,
                                 G_CALLBACK(on_gctx_focus_2hop), gv));
    gtk_box_append(GTK_BOX(box),
                   make_gctx_btn("Center on Node", "F", G_CALLBACK(on_gctx_center), gv));
    if (has_note)
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn("Copy [[wikilink]]", NULL,
                                   G_CALLBACK(on_gctx_copy_wikilink), gv));
    if (is_folder || nd->parent_idx >= 0) {
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn("Folder Color…", NULL, G_CALLBACK(on_gctx_folder_color), gv));
      gtk_box_append(GTK_BOX(box),
                     make_gctx_btn("Hide Folder", NULL, G_CALLBACK(on_gctx_hide_folder), gv));
    }
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
    gtk_box_append(GTK_BOX(box),
                   make_gctx_btn("Show All Hidden Folders", NULL,
                                 G_CALLBACK(on_gctx_show_all_folders), gv));
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
  gv->btn_2d = NULL;
  gv->btn_3d = NULL;
  gv->btn_galaxy = NULL;
  gv->btn_timeline = NULL;
  gv->tl_controls_box = NULL;
  gv->btn_tl_organic = NULL;
  gv->btn_tl_kept = NULL;
  gv->btn_tl_lane_folder = NULL;
  gv->btn_tl_lane_type = NULL;
  gv->btn_q_low = NULL;
  gv->btn_q_high = NULL;
  gv->btn_pulse = NULL;
  gv->btn_motion = NULL;
  gv->btn_tags = NULL;
  gv->btn_ghosts = NULL;
  gv->btn_links = NULL;
  gv->btn_col_folder = NULL;
  gv->btn_col_type = NULL;
  gv->legend_box = NULL;
  gv->legend_folders_box = NULL;
  gv->legend_lbl = NULL;
  gv->hover_lbl = NULL;
  gv->debug_lbl = NULL;
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
  gv->color_by_folder = TRUE;
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

  /* Top-left graph toolbar: View: 2D | 3D, Quality: Low | High, Tags, Ghosts, Pulse, Reduce motion. */
  GtkWidget *mode_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_name(mode_bar, "bt-graph-toolbar");
  gtk_widget_add_css_class(mode_bar, "bt-graph-controls");
  gtk_widget_set_halign(mode_bar, GTK_ALIGN_START);
  gtk_widget_set_valign(mode_bar, GTK_ALIGN_START);
  gtk_widget_set_margin_top(mode_bar, 10);
  gtk_widget_set_margin_start(mode_bar, 10);

  GtkWidget *lbl_view = gtk_label_new("View:");
  gtk_widget_add_css_class(lbl_view, "bt-graph-toolbar-lbl");
  gv->btn_2d = gtk_button_new_with_label("2D");
  gtk_widget_add_css_class(gv->btn_2d, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_2d, "Flat 2D planar graph view (drag to pan, scroll to zoom)");
  g_signal_connect(gv->btn_2d, "clicked", G_CALLBACK(on_btn_2d_clicked), gv);

  gv->btn_3d = gtk_button_new_with_label("3D");
  gtk_widget_add_css_class(gv->btn_3d, "bt-graph-pill");
  g_signal_connect(gv->btn_3d, "clicked", G_CALLBACK(on_btn_3d_clicked), gv);

  gv->btn_galaxy = gtk_button_new_with_label("Galaxy");
  gtk_widget_add_css_class(gv->btn_galaxy, "bt-graph-pill");
  g_signal_connect(gv->btn_galaxy, "clicked", G_CALLBACK(on_btn_galaxy_clicked), gv);

  gv->btn_timeline = gtk_button_new_with_label("Timeline");
  gtk_widget_add_css_class(gv->btn_timeline, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_timeline,
                              "Chronological Timeline view (wavy lanes, Organic/Kept, brush, ledger)");
  g_signal_connect(gv->btn_timeline, "clicked", G_CALLBACK(on_btn_timeline_clicked), gv);

  /* Timeline-specific sub-controls (visible only in Timeline mode). */
  gv->tl_controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *tl_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(tl_sep, "bt-graph-toolbar-sep");
  gv->btn_tl_organic = gtk_button_new_with_label("Organic");
  gtk_widget_add_css_class(gv->btn_tl_organic, "bt-graph-pill");
  g_signal_connect(gv->btn_tl_organic, "clicked", G_CALLBACK(on_btn_tl_organic_clicked), gv);
  gv->btn_tl_kept = gtk_button_new_with_label("Kept");
  gtk_widget_add_css_class(gv->btn_tl_kept, "bt-graph-pill");
  g_signal_connect(gv->btn_tl_kept, "clicked", G_CALLBACK(on_btn_tl_kept_clicked), gv);
  GtkWidget *lbl_lane = gtk_label_new("Lane:");
  gtk_widget_add_css_class(lbl_lane, "bt-graph-toolbar-lbl");
  gv->btn_tl_lane_folder = gtk_button_new_with_label("Folder");
  gtk_widget_add_css_class(gv->btn_tl_lane_folder, "bt-graph-pill");
  g_signal_connect(gv->btn_tl_lane_folder, "clicked", G_CALLBACK(on_btn_tl_lane_folder_clicked), gv);
  gv->btn_tl_lane_type = gtk_button_new_with_label("Type");
  gtk_widget_add_css_class(gv->btn_tl_lane_type, "bt-graph-pill");
  g_signal_connect(gv->btn_tl_lane_type, "clicked", G_CALLBACK(on_btn_tl_lane_type_clicked), gv);
  gtk_box_append(GTK_BOX(gv->tl_controls_box), tl_sep);
  gtk_box_append(GTK_BOX(gv->tl_controls_box), gv->btn_tl_organic);
  gtk_box_append(GTK_BOX(gv->tl_controls_box), gv->btn_tl_kept);
  gtk_box_append(GTK_BOX(gv->tl_controls_box), lbl_lane);
  gtk_box_append(GTK_BOX(gv->tl_controls_box), gv->btn_tl_lane_folder);
  gtk_box_append(GTK_BOX(gv->tl_controls_box), gv->btn_tl_lane_type);

  GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(sep1, "bt-graph-toolbar-sep");

  GtkWidget *lbl_qual = gtk_label_new("Quality:");
  gtk_widget_add_css_class(lbl_qual, "bt-graph-toolbar-lbl");
  gv->btn_q_low = gtk_button_new_with_label("Low");
  gtk_widget_add_css_class(gv->btn_q_low, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_q_low,
                              "Low quality: faster rendering without bloom or curved edges");
  g_signal_connect(gv->btn_q_low, "clicked", G_CALLBACK(on_btn_q_low_clicked), gv);

  gv->btn_q_high = gtk_button_new_with_label("High");
  gtk_widget_add_css_class(gv->btn_q_high, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_q_high,
                              "High quality: bloom glow, curved additive edges, and ambient dust");
  g_signal_connect(gv->btn_q_high, "clicked", G_CALLBACK(on_btn_q_high_clicked), gv);

  GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(sep2, "bt-graph-toolbar-sep");

  gv->btn_pulse = gtk_toggle_button_new_with_label("Pulse");
  gtk_widget_add_css_class(gv->btn_pulse, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_pulse,
                              "Animate subtle light ripples along edges from the selected node");
  g_signal_connect(gv->btn_pulse, "toggled", G_CALLBACK(on_btn_pulse_toggled), gv);

  gv->btn_motion = gtk_toggle_button_new_with_label("Reduce motion");
  gtk_widget_add_css_class(gv->btn_motion, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_motion,
                              "Reduce motion: disables dust drift, pulses, and 3D idle rotation");
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

  gv->btn_links = gtk_toggle_button_new_with_label("Links");
  gtk_widget_add_css_class(gv->btn_links, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_links,
                              "Show all [[wiki-link]] edges (when off, 1-hop links appear on hover/select)");
  g_signal_connect(gv->btn_links, "toggled", G_CALLBACK(on_btn_links_toggled), gv);

  GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(sep3, "bt-graph-toolbar-sep");

  GtkWidget *lbl_col = gtk_label_new("Color by:");
  gtk_widget_add_css_class(lbl_col, "bt-graph-toolbar-lbl");
  gv->btn_col_folder = gtk_button_new_with_label("Folder");
  gtk_widget_add_css_class(gv->btn_col_folder, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_col_folder, "Color nodes by top-level folder (Galaxy palette)");
  g_signal_connect(gv->btn_col_folder, "clicked", G_CALLBACK(on_btn_col_folder_clicked), gv);

  gv->btn_col_type = gtk_button_new_with_label("Type");
  gtk_widget_add_css_class(gv->btn_col_type, "bt-graph-pill");
  gtk_widget_set_tooltip_text(gv->btn_col_type, "Color nodes by type (Note, AI, Tag, Ghost, Session)");
  g_signal_connect(gv->btn_col_type, "clicked", G_CALLBACK(on_btn_col_type_clicked), gv);

  gtk_box_append(GTK_BOX(mode_bar), lbl_view);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_2d);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_3d);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_galaxy);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_timeline);
  gtk_box_append(GTK_BOX(mode_bar), gv->tl_controls_box);
  gtk_box_append(GTK_BOX(mode_bar), sep1);
  gtk_box_append(GTK_BOX(mode_bar), lbl_qual);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_q_low);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_q_high);
  gtk_box_append(GTK_BOX(mode_bar), sep2);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_links);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_tags);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_ghosts);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_pulse);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_motion);
  gtk_box_append(GTK_BOX(mode_bar), sep3);
  gtk_box_append(GTK_BOX(mode_bar), lbl_col);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_col_folder);
  gtk_box_append(GTK_BOX(mode_bar), gv->btn_col_type);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), mode_bar);

  /* Corner uppercase tracked mode badge (e.g. "GALAXY VIEW") for unmistakable mode feedback. */
  gv->mode_badge_lbl = gtk_label_new("2D VIEW");
  gtk_widget_set_name(gv->mode_badge_lbl, "bt-graph-mode-badge");
  gtk_widget_add_css_class(gv->mode_badge_lbl, "bt-graph-mode-badge");
  gtk_widget_set_halign(gv->mode_badge_lbl, GTK_ALIGN_START);
  gtk_widget_set_valign(gv->mode_badge_lbl, GTK_ALIGN_START);
  gtk_widget_set_margin_top(gv->mode_badge_lbl, 46);
  gtk_widget_set_margin_start(gv->mode_badge_lbl, 10);
  gtk_widget_set_can_target(gv->mode_badge_lbl, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), gv->mode_badge_lbl);

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

  gv->legend_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(gv->legend_box, "bt-graph-legend");
  gtk_widget_set_halign(gv->legend_box, GTK_ALIGN_START);
  gtk_widget_set_valign(gv->legend_box, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(gv->legend_box, 10);
  gtk_widget_set_margin_start(gv->legend_box, 10);

  GtkWidget *leg_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(leg_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(leg_scroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(leg_scroll), 150);
  gv->legend_folders_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(leg_scroll), gv->legend_folders_box);
  gtk_box_append(GTK_BOX(gv->legend_box), leg_scroll);

  gv->legend_lbl = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(gv->legend_lbl), 0.0f);
  gtk_box_append(GTK_BOX(gv->legend_box), gv->legend_lbl);
  update_legend(gv);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), gv->legend_box);

  gv->hover_lbl = gtk_label_new("");
  gtk_widget_set_name(gv->hover_lbl, "bt-graph-hover-readout");
  gtk_widget_add_css_class(gv->hover_lbl, "bt-graph-legend");
  gtk_label_set_ellipsize(GTK_LABEL(gv->hover_lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(gv->hover_lbl), 52);
  gtk_widget_set_halign(gv->hover_lbl, GTK_ALIGN_END);
  gtk_widget_set_valign(gv->hover_lbl, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(gv->hover_lbl, 10);
  gtk_widget_set_margin_end(gv->hover_lbl, 10);
  gtk_widget_set_can_target(gv->hover_lbl, FALSE);
  gtk_widget_set_visible(gv->hover_lbl, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), gv->hover_lbl);

  gv->debug_lbl = gtk_label_new("");
  gtk_widget_set_name(gv->debug_lbl, "bt-graph-debug");
  gtk_widget_add_css_class(gv->debug_lbl, "bt-graph-debug");
  gtk_label_set_xalign(GTK_LABEL(gv->debug_lbl), 0.0f);
  gtk_widget_set_halign(gv->debug_lbl, GTK_ALIGN_START);
  gtk_widget_set_valign(gv->debug_lbl, GTK_ALIGN_START);
  gtk_widget_set_margin_top(gv->debug_lbl, 44);
  gtk_widget_set_margin_start(gv->debug_lbl, 10);
  gtk_widget_set_can_target(gv->debug_lbl, FALSE);
  gtk_widget_set_visible(gv->debug_lbl, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(gv->overlay), gv->debug_lbl);

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
  g_free(gv->pts3d_from);
  g_free(gv->pts3d_target);
  g_free(gv->pending_notice);
  g_free(gv->nbr_mask);
  g_free(gv->pick_scratch);
  bt_folder_colors_free(gv->folder_colors);
  bt_timeline_free(gv->timeline);
  g_free(gv->vault_root_path);
  g_free(gv->isolated_folder);
  g_free(gv->color_target_folder);
  bt_graph_free(gv->graph);
  g_free(gv);
}

GtkWidget *bt_graphview_widget(BtGraphView *gv)
{
  return gv->root;
}

void bt_graphview_set_vault_root(BtGraphView *gv, const gchar *vault_root)
{
  if (gv == NULL)
    return;
  if (g_strcmp0(gv->vault_root_path, vault_root) == 0)
    return;
  g_free(gv->vault_root_path);
  gv->vault_root_path = g_strdup(vault_root);
  bt_folder_colors_free(gv->folder_colors);
  gv->folder_colors = bt_folder_colors_load(vault_root);
  g_clear_pointer(&gv->isolated_folder, g_free);
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
  gv->focus_isolated = FALSE;
  gv->focus_hops = 1u;
  gv->stack_count = 0u;
  gv->stack_total = 0u;
  gv->hover_fade = 0.0;
  gv->needs_fit = TRUE;
  gv->tl_dirty = TRUE;
  gv->tl_view_min = 0;
  gv->tl_view_max = 0;
  gv->tl_brush_t0 = 0;
  gv->tl_brush_t1 = 0;
  /* Hide wikilink clutter by default in dense views (>80 nodes); user can toggle "Links". */
  gv->show_links = (graph == NULL || graph->nodes->len <= 80u);
  apply_folder_colors_and_refresh(gv, FALSE);
  apply_kind_visibility(gv, FALSE);
  sync_toolbar_buttons(gv);
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
  if (mode_is_3d_orbit(mode) && gv->active_backend != BT_GRAPH_BACKEND_GL)
    mode = BT_GRAPH_MODE_2D;
  BtGraphMode prev = gv->mode;
  gv->mode = mode;

  /* Switch canvas stack: Timeline renders crisp editorial 2D on Cairo; others use GL when ready. */
  if (gv->canvas_stack != NULL) {
    if (mode == BT_GRAPH_MODE_TIMELINE) {
      gv->tl_dirty = TRUE;
      gtk_stack_set_visible_child_name(GTK_STACK(gv->canvas_stack), "cairo");
    } else if (gv->active_backend == BT_GRAPH_BACKEND_GL && gv->gl != NULL) {
      gtk_stack_set_visible_child_name(GTK_STACK(gv->canvas_stack), "gl");
    }
  }

  if (mode_is_3d_orbit(mode)) {
    gv->user_interacted_3d = FALSE;
    if (mode == BT_GRAPH_MODE_GALAXY && prev != BT_GRAPH_MODE_GALAXY) {
      gv->target_yaw = 0.35;
      gv->target_pitch = 0.58; /* angled down onto Andromeda spiral disk */
      gv->target_dist3d = 2.65;
      if (gv->reduce_motion) {
        gv->yaw = gv->target_yaw;
        gv->pitch = gv->target_pitch;
        gv->dist3d = gv->target_dist3d;
        gv->animating_3d = FALSE;
      } else {
        gv->animating_3d = TRUE;
      }
    }
    if (prev != mode)
      schedule_layout3d_worker(gv);
  }
  sync_toolbar_buttons(gv);
  if (needs_ambient_tick(gv) || gv->animating_3d || gv->morphing_3d)
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

void bt_graphview_set_show_links(BtGraphView *gv, gboolean show_links)
{
  if (gv == NULL)
    return;
  gv->show_links = show_links;
  sync_toolbar_buttons(gv);
  queue_canvas_draw(gv);
}

gboolean bt_graphview_get_show_links(const BtGraphView *gv)
{
  return gv ? gv->show_links : FALSE;
}

void bt_graphview_set_select_cb(BtGraphView *gv, BtGraphSelectNoteFn on_select, gpointer user_data)
{
  if (gv == NULL)
    return;
  gv->on_select = on_select;
  gv->select_ud = user_data;
}

void bt_graphview_select_relpath(BtGraphView *gv, const gchar *relpath)
{
  if (gv == NULL || gv->graph == NULL || relpath == NULL)
    return;
  for (guint i = 0; i < gv->graph->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (nd->relpath != NULL && g_ascii_strcasecmp(nd->relpath, relpath) == 0) {
      if (gv->selected_idx != (gint)i) {
        gv->selected_idx = (gint)i;
        refresh_neighbor_mask(gv);
        sync_toolbar_buttons(gv);
        queue_canvas_draw(gv);
      }
      return;
    }
  }
}

void bt_graphview_set_prefs_cb(BtGraphView *gv, BtGraphPrefsChangedFn on_prefs, gpointer user_data)
{
  if (gv == NULL)
    return;
  gv->on_prefs = on_prefs;
  gv->prefs_ud = user_data;
}

void bt_graphview_set_debug_overlay(BtGraphView *gv, gboolean enabled)
{
  if (gv == NULL)
    return;
  gv->debug_overlay = enabled ? TRUE : FALSE;
  if (gv->debug_lbl != NULL) {
    gtk_widget_set_visible(gv->debug_lbl, gv->debug_overlay);
    if (gv->debug_overlay)
      update_debug_overlay(gv, 0);
  }
}

gboolean bt_graphview_get_debug_overlay(const BtGraphView *gv)
{
  return gv ? gv->debug_overlay : FALSE;
}

void bt_graphview_toggle_debug_overlay(BtGraphView *gv)
{
  if (gv != NULL)
    bt_graphview_set_debug_overlay(gv, !gv->debug_overlay);
}

gboolean bt_graphview_is_screenshot_ready(const BtGraphView *gv)
{
  if (gv == NULL || gv->graph == NULL || gv->canvas_stack == NULL)
    return FALSE;
  if (gtk_widget_get_width(gv->canvas_stack) <= 40 ||
      gtk_widget_get_height(gv->canvas_stack) <= 40)
    return FALSE;
  if (gv->mode == BT_GRAPH_MODE_TIMELINE)
    return TRUE;
  if (gv->active_backend == BT_GRAPH_BACKEND_GL) {
    if (gv->gl == NULL || (!bt_graph_gl_is_ready(gv->gl) && !bt_graph_gl_has_failed(gv->gl)))
      return FALSE;
    if (mode_is_3d_orbit(gv->mode) && gv->graph->nodes->len > 0 &&
        (gv->pts3d == NULL || gv->pts3d_count == 0))
      return FALSE;
  }
  return TRUE;
}

void bt_graphview_prepare_screenshot(BtGraphView *gv)
{
  if (gv == NULL || gv->graph == NULL)
    return;
  guint n = gv->graph->nodes->len;
  for (guint i = 0; i < n; i++) {
    BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    nd->birth_t = 1.0;
  }
  if (gv->morphing_3d && gv->pts3d_target != NULL && gv->pts3d != NULL) {
    memcpy(gv->pts3d, gv->pts3d_target, gv->pts3d_count * sizeof(BtGraphVec3));
    gv->morphing_3d = FALSE;
    gv->morph_t = 1.0;
    g_clear_pointer(&gv->pts3d_from, g_free);
    g_clear_pointer(&gv->pts3d_target, g_free);
    if (gv->gl != NULL)
      bt_graph_gl_set_layout3d(gv->gl, gv->pts3d, gv->pts3d_count);
  }
  gv->animating_cam = FALSE;
  gv->animating_3d = FALSE;
  if (gv->mode == BT_GRAPH_MODE_GALAXY) {
    gv->yaw = 0.35;
    gv->pitch = 0.58;
    gv->dist3d = 2.55;
  } else if (gv->mode == BT_GRAPH_MODE_3D) {
    gv->yaw = 0.35;
    gv->pitch = 0.22;
    gv->dist3d = 2.75;
  } else if (gv->mode == BT_GRAPH_MODE_2D) {
    int w = gv->canvas_stack ? gtk_widget_get_width(gv->canvas_stack) : 800;
    int h = gv->canvas_stack ? gtk_widget_get_height(gv->canvas_stack) : 600;
    compute_fit_camera(gv, w, h, &gv->pan_x, &gv->pan_y, &gv->zoom);
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
    gv->target_zoom = gv->zoom;
    gv->needs_fit = FALSE;
  } else if (gv->mode == BT_GRAPH_MODE_TIMELINE) {
    gv->tl_dirty = TRUE;
  }
  queue_canvas_draw(gv);
}

gboolean bt_graphview_is_capture_ready(const BtGraphView *gv, const BtGraphCaptureSpec *spec)
{
  if (gv == NULL || gv->graph == NULL || gv->canvas_stack == NULL)
    return FALSE;
  if (!gtk_widget_get_mapped(gv->canvas_stack))
    return FALSE;
  if (gtk_widget_get_width(gv->canvas_stack) <= 40 ||
      gtk_widget_get_height(gv->canvas_stack) <= 40)
    return FALSE;
  if (gv->active_backend == BT_GRAPH_BACKEND_GL) {
    if (gv->gl == NULL || (!bt_graph_gl_is_ready(gv->gl) && !bt_graph_gl_has_failed(gv->gl)))
      return FALSE;
    if (spec != NULL && spec->mode == BT_GRAPH_MODE_3D && gv->graph->nodes->len > 0 &&
        (gv->pts3d == NULL || gv->pts3d_count < gv->graph->nodes->len))
      return FALSE;
  }
  return TRUE;
}

void bt_graphview_apply_capture_state(BtGraphView *gv, const BtGraphCaptureSpec *spec)
{
  if (gv == NULL || gv->graph == NULL || spec == NULL)
    return;

  bt_graphview_set_show_tags(gv, spec->show_tags);
  bt_graphview_set_show_ghosts(gv, spec->show_ghosts);
  bt_graphview_set_quality(gv, spec->quality);
  bt_graphview_set_mode(gv, spec->mode);
  bt_graphview_set_reduce_motion(gv, spec->reduce_motion);

  guint n = gv->graph->nodes->len;
  for (guint i = 0; i < n; i++) {
    BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    nd->birth_t = 1.0;
  }

  int w = gv->canvas_stack ? gtk_widget_get_width(gv->canvas_stack) : 800;
  int h = gv->canvas_stack ? gtk_widget_get_height(gv->canvas_stack) : 600;
  gv->animating_cam = FALSE;
  gv->animating_3d = FALSE;
  gv->needs_fit = FALSE;

  if (spec->cam == BT_CAP_CAM_FIT) {
    compute_fit_camera(gv, w, h, &gv->pan_x, &gv->pan_y, &gv->zoom);
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
    gv->target_zoom = gv->zoom;
  } else if (spec->cam == BT_CAP_CAM_ZOOM2X_DENSEST) {
    gdouble dense_x = 0.0, dense_y = 0.0;
    guint best_cnt = 0;
    for (guint i = 0; i < n; i++) {
      const BtGraphNode *a = g_ptr_array_index(gv->graph->nodes, i);
      if (!a->visible) continue;
      guint cnt = 0;
      gdouble sx = 0.0, sy = 0.0;
      for (guint j = 0; j < n; j++) {
        const BtGraphNode *b = g_ptr_array_index(gv->graph->nodes, j);
        if (!b->visible) continue;
        if (hypot(a->x - b->x, a->y - b->y) <= 45.0) {
          cnt++;
          sx += b->x;
          sy += b->y;
        }
      }
      if (cnt > best_cnt) {
        best_cnt = cnt;
        dense_x = sx / (gdouble)cnt;
        dense_y = sy / (gdouble)cnt;
      }
    }
    gv->zoom = 2.0;
    gv->pan_x = -dense_x * 2.0;
    gv->pan_y = -dense_y * 2.0;
    gv->target_zoom = gv->zoom;
    gv->target_pan_x = gv->pan_x;
    gv->target_pan_y = gv->pan_y;
  } else if (spec->cam == BT_CAP_CAM_3D_ORBIT) {
    const gchar *yaw_env = g_getenv("BT_CAPTURE_YAW");
    gv->yaw = (yaw_env != NULL && yaw_env[0] != '\0') ? g_ascii_strtod(yaw_env, NULL) : 0.35;
    gv->pitch = 0.20;
    gv->dist3d = 2.8;
    gv->target_yaw = gv->yaw;
    gv->target_pitch = gv->pitch;
    gv->target_dist3d = gv->dist3d;
    gv->user_interacted_3d = TRUE;
  }

  gint hub_idx = -1, small_idx = -1;
  guint hub_deg = 0;
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(gv->graph->nodes, i);
    if (!nd->visible) continue;
    if ((nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE) && nd->degree > hub_deg) {
      hub_deg = nd->degree;
      hub_idx = (gint)i;
    }
    if (small_idx < 0 &&
        (nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE) &&
        (nd->degree == 1u || nd->degree == 2u)) {
      small_idx = (gint)i;
    }
  }

  gv->focus_idx = -1;
  switch (spec->interact) {
  case BT_CAP_INTERACT_HOVER_HUB:
    gv->hover_idx = hub_idx;
    gv->selected_idx = -1;
    gv->hover_fade = (hub_idx >= 0) ? 1.0 : 0.0;
    break;
  case BT_CAP_INTERACT_HOVER_SMALL:
    gv->hover_idx = small_idx;
    gv->selected_idx = -1;
    gv->hover_fade = (small_idx >= 0) ? 1.0 : 0.0;
    break;
  case BT_CAP_INTERACT_SELECT_HUB:
    gv->hover_idx = -1;
    gv->selected_idx = hub_idx;
    gv->hover_fade = (hub_idx >= 0) ? 1.0 : 0.0;
    break;
  case BT_CAP_INTERACT_NONE:
  default:
    gv->hover_idx = -1;
    gv->selected_idx = -1;
    gv->hover_fade = 0.0;
    break;
  }

  gv->time_sec = spec->reduce_motion ? 0.0 : 4.0;
  refresh_neighbor_mask(gv);
  update_hover_readout(gv);
  sync_toolbar_buttons(gv);
  queue_canvas_draw(gv);
}

static guint current_overlap_count(const BtGraphView *gv)
{
  if (gv == NULL)
    return 0u;
  if (gv->active_backend == BT_GRAPH_BACKEND_GL && gv->gl != NULL)
    return bt_graph_gl_last_overlap_count(gv->gl);
  if (gv->cairo_renderer != NULL && gv->cairo_renderer->priv != NULL)
    return ((const BtCairoPriv *)gv->cairo_renderer->priv)->last_overlap_count;
  return 0u;
}

gboolean bt_graphview_capture_evidence(BtGraphView *gv, GtkWindow *win, const gchar *out_dir,
                                       const BtGraphCaptureSpec *spec, const gchar *build_stamp,
                                       GError **error)
{
  if (gv == NULL || gv->canvas_stack == NULL || out_dir == NULL || spec == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid capture arguments");
    return FALSE;
  }
  if (g_mkdir_with_parents(out_dir, 0755) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create output dir '%s'", out_dir);
    return FALSE;
  }

  int gw = gtk_widget_get_width(gv->canvas_stack);
  int gh = gtk_widget_get_height(gv->canvas_stack);
  int scale = MAX(gtk_widget_get_scale_factor(gv->canvas_stack), 1);
  if (gw <= 0 || gh <= 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Graph widget has zero size");
    return FALSE;
  }

  BtGraphRenderState st;
  fill_render_state(gv, gw, gh, scale, &st);

  gchar *png_name = g_strdup_printf("%s.png", spec->name);
  gchar *png_path = g_build_filename(out_dir, png_name, NULL);
  g_free(png_name);

  gboolean is_gl = (gv->active_backend == BT_GRAPH_BACKEND_GL && gv->gl != NULL &&
                    bt_graph_gl_is_ready(gv->gl));
  gboolean ok = FALSE;
  if (is_gl) {
    ok = bt_graph_gl_capture_png(gv->gl, &st, png_path, error);
  } else {
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw * scale, gh * scale);
    cairo_t *cr = cairo_create(surf);
    cairo_scale(cr, (gdouble)scale, (gdouble)scale);
    if (gv->cairo_renderer != NULL && gv->cairo_renderer->vt->render_cairo != NULL)
      gv->cairo_renderer->vt->render_cairo(gv->cairo_renderer, cr, &st);
    cairo_destroy(cr);
    cairo_status_t cst = cairo_surface_write_to_png(surf, png_path);
    cairo_surface_destroy(surf);
    if (cst == CAIRO_STATUS_SUCCESS)
      ok = TRUE;
    else
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cairo PNG write failed: %s",
                  cairo_status_to_string(cst));
  }
  g_free(png_path);
  if (!ok)
    return FALSE;

  guint vn = 0, tn = 0, ve = 0, te = 0;
  count_visible_graph_stats(gv, &vn, &tn, &ve, &te);
  guint drawn_lbl = current_drawn_label_count(gv);
  guint ov_cnt = current_overlap_count(gv);

  int ww = win ? gtk_widget_get_width(GTK_WIDGET(win)) : gw;
  int wh = win ? gtk_widget_get_height(GTK_WIDGET(win)) : gh;

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "state", spec->name);
  cJSON_AddStringToObject(root, "build_stamp", build_stamp ? build_stamp : "");
  cJSON_AddNumberToObject(root, "window_width", ww);
  cJSON_AddNumberToObject(root, "window_height", wh);
  cJSON_AddNumberToObject(root, "graph_width", gw);
  cJSON_AddNumberToObject(root, "graph_height", gh);
  cJSON_AddNumberToObject(root, "scale_factor", scale);
  cJSON_AddStringToObject(root, "backend", is_gl ? "gl" : "cairo");
  cJSON_AddStringToObject(root, "gl_renderer",
                          is_gl ? bt_graph_gl_renderer(gv->gl) : "CPU (Cairo fallback)");
  cJSON_AddStringToObject(root, "gl_version", is_gl ? bt_graph_gl_version(gv->gl) : "N/A");
  cJSON_AddStringToObject(root, "quality",
                          (gv->quality == BT_GRAPH_QUALITY_HIGH) ? "high" : "low");
  cJSON_AddStringToObject(root, "mode",
                          (is_gl && gv->mode == BT_GRAPH_MODE_3D) ? "3d" : "2d");
  cJSON_AddBoolToObject(root, "reduce_motion", gv->reduce_motion);
  cJSON_AddNumberToObject(root, "nodes_total", (double)tn);
  cJSON_AddNumberToObject(root, "nodes_visible", (double)vn);
  cJSON_AddNumberToObject(root, "edges_total", (double)te);
  cJSON_AddNumberToObject(root, "edges_visible", (double)ve);
  cJSON_AddNumberToObject(root, "drawn_labels", (double)drawn_lbl);
  cJSON_AddNumberToObject(root, "overlap_count", (double)ov_cnt);
  cJSON_AddNumberToObject(root, "settle_frames", (double)spec->settle_frames);

  gchar *json_name = g_strdup_printf("%s.json", spec->name);
  gchar *json_path = g_build_filename(out_dir, json_name, NULL);
  g_free(json_name);

  char *printed = cJSON_Print(root);
  cJSON_Delete(root);
  gboolean jok = g_file_set_contents(json_path, printed ? printed : "{}", -1, error);
  free(printed);
  g_free(json_path);
  return jok;
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
