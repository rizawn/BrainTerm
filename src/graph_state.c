/* graph_state.c — Pure-C (GTK-free) Brain Graph v2 backend selection,
 * hover-neighbor mask, label visibility rules, and 3D cortex projection. */
#include "graph_state.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void bt_graph_select_backend(const BtGraphGlProbe *probe, BtGraphBackendDecision *out)
{
  if (out == NULL)
    return;
  memset(out, 0, sizeof(*out));
  out->backend = BT_GRAPH_BACKEND_CAIRO;
  out->bloom_enabled = FALSE;
  out->mode_3d_available = FALSE;

  if (probe == NULL) {
    g_strlcpy(out->reason, "OpenGL probe unavailable", sizeof(out->reason));
    return;
  }
  if (probe->force_cairo_env) {
    g_strlcpy(out->reason, "forced Cairo via BRAINTERM_GRAPH_BACKEND=cairo", sizeof(out->reason));
    return;
  }
  if (!probe->gl_context_ok) {
    g_strlcpy(out->reason, "OpenGL context unavailable", sizeof(out->reason));
    return;
  }
  if (probe->is_gles) {
    if (probe->gl_major < 3) {
      g_snprintf(out->reason, sizeof(out->reason),
                 "OpenGL ES %d.%d too old (need >= 3.0)", probe->gl_major, probe->gl_minor);
      return;
    }
  } else {
    if (probe->gl_major < 3 || (probe->gl_major == 3 && probe->gl_minor < 3)) {
      g_snprintf(out->reason, sizeof(out->reason),
                 "OpenGL %d.%d too old (need >= 3.3)", probe->gl_major, probe->gl_minor);
      return;
    }
  }
  if (!probe->shaders_ok) {
    g_strlcpy(out->reason, "GLSL shader compilation failed", sizeof(out->reason));
    return;
  }

  out->backend = BT_GRAPH_BACKEND_GL;
  out->mode_3d_available = TRUE;
  out->bloom_enabled = probe->fbo_ok;
  if (!probe->fbo_ok)
    g_strlcpy(out->reason, "bloom disabled (FBO unsupported)", sizeof(out->reason));
  else
    out->reason[0] = '\0';
}

void bt_graph_compute_neighbors(const BtGraph *g, gint focus_idx, guint8 *mask, guint mask_len)
{
  if (mask == NULL || mask_len == 0)
    return;
  memset(mask, BT_NBR_NONE, mask_len);
  if (g == NULL || g->nodes == NULL || focus_idx < 0 || (guint)focus_idx >= g->nodes->len)
    return;

  guint f = (guint)focus_idx;
  if (f < mask_len)
    mask[f] = BT_NBR_FOCUS;

  if (g->edges == NULL)
    return;
  for (guint i = 0; i < g->edges->len; i++) {
    const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
    if (e->src_idx == f && e->dst_idx != f && e->dst_idx < mask_len)
      mask[e->dst_idx] = BT_NBR_NEIGHBOR;
    else if (e->dst_idx == f && e->src_idx != f && e->src_idx < mask_len)
      mask[e->src_idx] = BT_NBR_NEIGHBOR;
  }
}

guint bt_graph_hub_degree_threshold(const BtGraph *g)
{
  if (g == NULL || g->nodes == NULL || g->nodes->len <= 24u)
    return 0u;

  /* Histogram over bounded degree buckets (0..64) -> O(N), zero malloc. */
  guint hist[65];
  memset(hist, 0, sizeof(hist));
  guint vis = 0;
  for (guint i = 0; i < g->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (!nd->visible)
      continue;
    guint b = MIN(nd->degree, 64u);
    hist[b]++;
    vis++;
  }
  if (vis <= 24u)
    return 0u;

  /* Target top ~10% hubs (at most ~32 hub labels on dense graphs), min degree 3. */
  guint target = MAX(vis / 10u, 8u);
  if (target > 32u)
    target = 32u;
  guint acc = 0;
  for (gint d = 64; d >= 3; d--) {
    acc += hist[d];
    if (acc >= target)
      return (guint)d;
  }
  return 3u;
}

gdouble bt_graph_label_alpha(const BtGraphNode *nd, guint8 nbr_state, gboolean is_selected,
                             gboolean is_kbd_focus, guint hub_thresh, gdouble zoom,
                             gboolean has_focus, gdouble focus_fade, guint total_nodes)
{
  if (nd == NULL || !nd->visible || nd->label == NULL || nd->label[0] == '\0')
    return 0.0;

  gdouble hf = CLAMP(focus_fade, 0.0, 1.0);
  if (is_selected || is_kbd_focus)
    return 1.0;
  if (nbr_state == BT_NBR_FOCUS)
    return 0.45 + 0.55 * hf;
  if (nbr_state == BT_NBR_NEIGHBOR)
    return 0.35 + 0.55 * hf;

  gdouble dim = has_focus ? (1.0 - 0.75 * hf) : 1.0;
  if (zoom < 0.45)
    return 0.0;

  gdouble zoom_fade = CLAMP((zoom - 0.45) / 0.45, 0.0, 1.0);
  if (total_nodes <= 32u) {
    if (nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE ||
        nd->degree >= hub_thresh)
      return 0.78 * zoom_fade * dim;
    return 0.0;
  }

  if (nd->degree >= hub_thresh && hub_thresh > 0u)
    return 0.78 * zoom_fade * dim;

  return 0.0;
}

guint bt_graph_label_cap_for_zoom(gdouble zoom)
{
  int cap = (int)llround(20.0 * CLAMP(zoom, 0.5, 2.5));
  return (guint)CLAMP(cap, 12, 48);
}

static gboolean cand_higher_priority(const BtGraphLabelCand *a, const BtGraphLabelCand *b)
{
  if (a->priority != b->priority)
    return a->priority > b->priority;
  if (a->degree != b->degree)
    return a->degree > b->degree;
  return a->node_idx < b->node_idx;
}

static gboolean aabb_overlaps(const BtGraphLabelCand *a, const BtGraphLabelCand *b,
                              float pad_x, float pad_y)
{
  return (a->x - pad_x < b->x + b->w + pad_x) &&
         (a->x + a->w + pad_x > b->x - pad_x) &&
         (a->y - pad_y < b->y + b->h + pad_y) &&
         (a->y + a->h + pad_y > b->y - pad_y);
}

static gboolean aabb_hits_core(const BtGraphLabelCand *c, const BtGraphCoreObstacle *o)
{
  float qx = CLAMP(o->cx, c->x, c->x + c->w);
  float qy = CLAMP(o->cy, c->y, c->y + c->h);
  float dx = qx - o->cx;
  float dy = qy - o->cy;
  float rr = o->r + 1.0f;
  return (dx * dx + dy * dy) < (rr * rr);
}

guint bt_graph_place_labels(BtGraphLabelCand *cands, guint cand_count,
                            const BtGraphCoreObstacle *obstacles, guint obstacle_count,
                            guint max_labels, guint max_neighbors,
                            BtGraphLabelCand *out, guint out_cap)
{
  if (cands == NULL || cand_count == 0 || out == NULL || out_cap == 0)
    return 0;

  /* Deterministic zero-alloc Shell sort by (priority desc, degree desc, node_idx asc). */
  for (guint gap = cand_count / 2u; gap > 0u; gap /= 2u) {
    for (guint i = gap; i < cand_count; i++) {
      BtGraphLabelCand tmp = cands[i];
      guint j = i;
      while (j >= gap && cand_higher_priority(&tmp, &cands[j - gap])) {
        cands[j] = cands[j - gap];
        j -= gap;
      }
      cands[j] = tmp;
    }
  }

  guint placed = 0;
  guint placed_neighbors = 0;

  for (guint i = 0; i < cand_count && placed < out_cap; i++) {
    BtGraphLabelCand c = cands[i];
    if (c.always_show) {
      /* Always show focused/selected/hovered; nudge down if colliding with prior always_show. */
      for (guint tries = 0; tries < 4u; tries++) {
        gboolean hit = FALSE;
        for (guint j = 0; j < placed; j++) {
          if (aabb_overlaps(&c, &out[j], 1.0f, 1.0f)) {
            c.y = out[j].y + out[j].h + 3.0f;
            hit = TRUE;
            break;
          }
        }
        if (!hit)
          break;
      }
      out[placed++] = c;
      continue;
    }

    if (placed >= max_labels)
      continue;
    if (c.is_neighbor && placed_neighbors >= max_neighbors)
      continue;

    gboolean collides = FALSE;
    for (guint j = 0; j < placed; j++) {
      if (aabb_overlaps(&c, &out[j], 2.0f, 1.0f)) {
        collides = TRUE;
        break;
      }
    }
    if (collides)
      continue;

    if (obstacles != NULL && obstacle_count > 0) {
      for (guint k = 0; k < obstacle_count; k++) {
        if (obstacles[k].node_idx == c.node_idx)
          continue;
        if (aabb_hits_core(&c, &obstacles[k])) {
          collides = TRUE;
          break;
        }
      }
      if (collides)
        continue;
    }

    out[placed++] = c;
    if (c.is_neighbor)
      placed_neighbors++;
  }

  return placed;
}

static guint32 hash_u32(guint32 x)
{
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

void bt_graph_compute_3d_layout(const BtGraph *g, BtGraphVec3 *out_pts, guint max_pts)
{
  if (g == NULL || g->nodes == NULL || out_pts == NULL || max_pts == 0)
    return;
  guint n = MIN(g->nodes->len, max_pts);

  /* Compute 2D extent so projection adapts cleanly to any vault scale. */
  gdouble max_r = 120.0;
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    gdouble r = hypot(nd->x, nd->y);
    if (r > max_r)
      max_r = r;
  }

  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    guint32 h1 = hash_u32(i * 2654435761u + (guint32)nd->degree * 31u + 17u);
    guint32 h2 = hash_u32(h1 ^ 0x9E3779B9u);
    gdouble u1 = (gdouble)(h1 & 0xFFFFu) / 65535.0; /* [0, 1] */
    gdouble u2 = (gdouble)(h2 & 0xFFFFu) / 65535.0; /* [0, 1] */

    if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT) {
      /* Brainstem dock at lower pole. */
      gdouble ang = (nd->x / max_r) * M_PI + u1 * 0.6;
      gdouble rad = 0.14 + 0.10 * u2;
      out_pts[i].x = (float)(cos(ang) * rad);
      out_pts[i].y = (float)(-0.72 - 0.14 * u1);
      out_pts[i].z = (float)(sin(ang) * rad);
    } else if ((nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE) &&
               nd->degree == 0u) {
      /* Isolated notes form a vertical luminous spine ascending into the cortex. */
      gdouble t = CLAMP((180.0 - nd->y) / 400.0, 0.0, 1.0);
      gdouble ang = u1 * 2.0 * M_PI;
      gdouble rad = 0.05 + 0.06 * u2;
      out_pts[i].x = (float)(cos(ang) * rad);
      out_pts[i].y = (float)(-0.55 + t * 0.70);
      out_pts[i].z = (float)(sin(ang) * rad);
    } else {
      /* Bilateral cortex ellipsoid derived from 2D angle/radius + deterministic depth. */
      gdouble nx = nd->x / max_r;
      gdouble ny = nd->y / max_r;
      gdouble r2 = CLAMP(hypot(nx, ny), 0.05, 1.0);
      gdouble theta = atan2(ny, nx);
      /* Subtle longitudinal fissure separates left/right lobes slightly along X. */
      gdouble side = (cos(theta) >= 0.0) ? 1.0 : -1.0;
      gdouble shell = 0.35 + 0.58 * pow(r2, 0.72);
      if (nd->kind == BT_NODE_TAG)
        shell *= 0.65;
      else if (nd->kind == BT_NODE_GHOST)
        shell = 0.92 + 0.06 * u2;

      /* Map onto oblate cortex dome above the spine. */
      gdouble phi = (u1 - 0.5) * M_PI * 0.78;
      gdouble cx = cos(theta) * cos(phi) * shell * 0.88 + side * 0.06;
      gdouble cy = (0.14 + sin(phi) * shell * 0.62) - ny * 0.14;
      gdouble cz = sin(theta) * cos(phi) * shell * 0.96;

      out_pts[i].x = (float)CLAMP(cx, -1.0, 1.0);
      out_pts[i].y = (float)CLAMP(cy, -0.50, 0.95);
      out_pts[i].z = (float)CLAMP(cz, -1.0, 1.0);
    }
  }
}

static void mat4_mul(const float a[16], const float b[16], float out[16])
{
  float r[16];
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
                       a[1 * 4 + row] * b[c * 4 + 1] +
                       a[2 * 4 + row] * b[c * 4 + 2] +
                       a[3 * 4 + row] * b[c * 4 + 3];
    }
  }
  memcpy(out, r, sizeof(r));
}

void bt_graph_build_orbit_mvp(gdouble yaw, gdouble pitch, gdouble dist, gdouble aspect,
                              float mvp_out[16])
{
  gdouble asp = MAX(aspect, 0.1);
  gdouble fov = 45.0 * (M_PI / 180.0);
  gdouble f = 1.0 / tan(fov * 0.5);
  gdouble zn = 0.1, zf = 20.0;

  /* Perspective matrix (column-major). */
  float proj[16];
  memset(proj, 0, sizeof(proj));
  proj[0] = (float)(f / asp);
  proj[5] = (float)f;
  proj[10] = (float)((zf + zn) / (zn - zf));
  proj[11] = -1.0f;
  proj[14] = (float)((2.0 * zf * zn) / (zn - zf));

  /* Camera orbits origin at (yaw, pitch, dist). */
  gdouble cp = cos(pitch), sp = sin(pitch);
  gdouble cy = cos(yaw), sy = sin(yaw);
  gdouble ex = dist * cp * sy;
  gdouble ey = dist * sp;
  gdouble ez = dist * cp * cy;

  /* LookAt(eye, (0,0,0), (0,1,0)) */
  gdouble fl = hypot(hypot(ex, ey), ez);
  gdouble fx = -ex / MAX(fl, 1e-6);
  gdouble fy = -ey / MAX(fl, 1e-6);
  gdouble fz = -ez / MAX(fl, 1e-6);

  /* side = normalize(cross(forward, up(0,1,0))) = (-fz, 0, fx) */
  gdouble sx = -fz, sy_v = 0.0, sz = fx;
  gdouble sl = hypot(sx, sz);
  if (sl < 1e-6) {
    sx = 1.0; sz = 0.0; sl = 1.0;
  }
  sx /= sl;
  sz /= sl;

  /* true up = cross(side, forward) */
  gdouble ux = sy_v * fz - sz * fy;
  gdouble uy = sz * fx - sx * fz;
  gdouble uz = sx * fy - sy_v * fx;

  float view[16];
  memset(view, 0, sizeof(view));
  view[0] = (float)sx;  view[4] = (float)sy_v; view[8]  = (float)sz;  view[12] = (float)-(sx * ex + sy_v * ey + sz * ez);
  view[1] = (float)ux;  view[5] = (float)uy;   view[9]  = (float)uz;  view[13] = (float)-(ux * ex + uy * ey + uz * ez);
  view[2] = (float)-fx; view[6] = (float)-fy;  view[10] = (float)-fz; view[14] = (float)(fx * ex + fy * ey + fz * ez);
  view[15] = 1.0f;

  mat4_mul(proj, view, mvp_out);
}

gboolean bt_graph_project_3d(const float mvp[16], const BtGraphVec3 *pt, gdouble vp_w,
                             gdouble vp_h, gdouble *sx_out, gdouble *sy_out, gdouble *w_out)
{
  if (mvp == NULL || pt == NULL)
    return FALSE;
  float x = pt->x, y = pt->y, z = pt->z;
  float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
  float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
  float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
  if (cw <= 0.15f)
    return FALSE;
  float nx = cx / cw;
  float ny = cy / cw;
  if (sx_out) *sx_out = ((gdouble)nx * 0.5 + 0.5) * vp_w;
  if (sy_out) *sy_out = (0.5 - (gdouble)ny * 0.5) * vp_h;
  if (w_out) *w_out = (gdouble)cw;
  return TRUE;
}
