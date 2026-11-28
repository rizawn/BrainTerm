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

void bt_graph_compute_neighbors_hops(const BtGraph *g, gint focus_idx, guint max_hops,
                                     guint8 *mask, guint mask_len)
{
  if (mask == NULL || mask_len == 0)
    return;
  memset(mask, BT_NBR_NONE, mask_len);
  if (g == NULL || g->nodes == NULL || focus_idx < 0 || (guint)focus_idx >= g->nodes->len)
    return;

  guint f = (guint)focus_idx;
  const BtGraphNode *fnd = g_ptr_array_index(g->nodes, f);
  if (!fnd->visible)
    return;
  if (f < mask_len)
    mask[f] = BT_NBR_FOCUS;

  if (g->edges == NULL || max_hops == 0u)
    return;

  gboolean focus_is_folder = (fnd->kind == BT_NODE_FOLDER);

  /* Hop 1 */
  for (guint i = 0; i < g->edges->len; i++) {
    const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
    if (e->kind == BT_EDGE_CONTAINS && !focus_is_folder)
      continue;
    if (e->src_idx == f && e->dst_idx != f && e->dst_idx < mask_len &&
        e->dst_idx < g->nodes->len) {
      const BtGraphNode *dn = g_ptr_array_index(g->nodes, e->dst_idx);
      if (dn->visible)
        mask[e->dst_idx] = BT_NBR_NEIGHBOR;
    } else if (e->dst_idx == f && e->src_idx != f && e->src_idx < mask_len &&
               e->src_idx < g->nodes->len) {
      const BtGraphNode *sn = g_ptr_array_index(g->nodes, e->src_idx);
      if (sn->visible)
        mask[e->src_idx] = BT_NBR_NEIGHBOR;
    }
  }

  /* Optional Hop 2 */
  if (max_hops >= 2u) {
    for (guint i = 0; i < g->edges->len; i++) {
      const BtGraphEdge *e = &g_array_index(g->edges, BtGraphEdge, i);
      if (e->kind == BT_EDGE_CONTAINS)
        continue;
      if (e->src_idx >= mask_len || e->dst_idx >= mask_len ||
          e->src_idx >= g->nodes->len || e->dst_idx >= g->nodes->len)
        continue;
      if (mask[e->src_idx] == BT_NBR_NEIGHBOR && mask[e->dst_idx] == BT_NBR_NONE) {
        const BtGraphNode *dn = g_ptr_array_index(g->nodes, e->dst_idx);
        if (dn->visible)
          mask[e->dst_idx] = BT_NBR_HOP2;
      } else if (mask[e->dst_idx] == BT_NBR_NEIGHBOR && mask[e->src_idx] == BT_NBR_NONE) {
        const BtGraphNode *sn = g_ptr_array_index(g->nodes, e->src_idx);
        if (sn->visible)
          mask[e->src_idx] = BT_NBR_HOP2;
      }
    }
  }
}

void bt_graph_compute_neighbors(const BtGraph *g, gint focus_idx, guint8 *mask, guint mask_len)
{
  bt_graph_compute_neighbors_hops(g, focus_idx, 1u, mask, mask_len);
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
  if (nbr_state == BT_NBR_HOP2)
    return 0.20 + 0.40 * hf;

  gdouble dim = has_focus ? (1.0 - 0.75 * hf) : 1.0;

  /* Folders are first-class landmarks: label always visible unless strongly dimmed. */
  if (nd->kind == BT_NODE_FOLDER) {
    gdouble a = 0.92 * dim;
    return (has_focus && a < 0.35) ? 0.0 : a;
  }

  if (zoom < 0.45)
    return 0.0;

  gdouble zoom_fade = CLAMP((zoom - 0.45) / 0.45, 0.0, 1.0);
  if (total_nodes <= 32u) {
    if (nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE ||
        nd->degree >= hub_thresh) {
      gdouble a = 0.78 * zoom_fade * dim;
      return (has_focus && a < 0.40) ? 0.0 : a;
    }
    return 0.0;
  }

  if (nd->degree >= hub_thresh && hub_thresh > 0u) {
    gdouble a = 0.78 * zoom_fade * dim;
    return (has_focus && a < 0.40) ? 0.0 : a;
  }

  return 0.0;
}

gdouble bt_graph_screen_core_radius(gdouble base_radius, gdouble zoom)
{
  return CLAMP(base_radius * pow(MAX(zoom, 0.05), 0.35), 3.0, 14.0);
}

void bt_graph_zoom_at_point(gdouble *pan_x, gdouble *pan_y, gdouble *zoom, gdouble new_zoom,
                            gdouble mouse_x, gdouble mouse_y, gdouble vp_w, gdouble vp_h)
{
  if (pan_x == NULL || pan_y == NULL || zoom == NULL || *zoom <= 1e-6)
    return;
  gdouble old_zoom = *zoom;
  gdouble clamped = CLAMP(new_zoom, 0.2, 4.0);
  gdouble factor = 1.0 - (clamped / old_zoom);
  *pan_x += (mouse_x - vp_w * 0.5 - *pan_x) * factor;
  *pan_y += (mouse_y - vp_h * 0.5 - *pan_y) * factor;
  *zoom = clamped;
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

static gboolean placement_collides(const BtGraphLabelCand *c,
                                   const BtGraphLabelCand *placed, guint placed_count,
                                   const BtGraphCoreObstacle *obstacles, guint obstacle_count)
{
  for (guint j = 0; j < placed_count; j++) {
    if (aabb_overlaps(c, &placed[j], 2.0f, 1.0f))
      return TRUE;
  }
  if (obstacles != NULL && obstacle_count > 0) {
    for (guint k = 0; k < obstacle_count; k++) {
      if (obstacles[k].node_idx == c->node_idx)
        continue;
      if (aabb_hits_core(c, &obstacles[k]))
        return TRUE;
    }
  }
  return FALSE;
}

static BtGraphLabelCand place_primary_label(BtGraphLabelCand c,
                                            const BtGraphLabelViewport *viewport,
                                            const BtGraphLabelCand *placed, guint placed_count,
                                            const BtGraphCoreObstacle *obstacles, guint obstacle_count)
{
  const float max_x = MAX(0.0f, viewport->width - c.w - 1.0f);
  const float max_y = MAX(0.0f, viewport->height - c.h - 1.0f);
  const float min_x = MIN(1.0f, max_x);
  const float min_y = MIN(1.0f, max_y);
  const float positions[][2] = {
    {c.x, c.y},
    {c.x, floorf(c.anchor_y - c.anchor_r - c.h - 5.0f)},
    {floorf(c.anchor_x + c.anchor_r + 6.0f), floorf(c.anchor_y - c.h * 0.5f)},
    {floorf(c.anchor_x - c.anchor_r - c.w - 6.0f), floorf(c.anchor_y - c.h * 0.5f)},
  };
  BtGraphLabelCand fallback = c;
  fallback.x = CLAMP(c.x, min_x, max_x);
  fallback.y = CLAMP(c.y, min_y, max_y);
  gboolean found_fit = FALSE;

  for (guint i = 0; i < G_N_ELEMENTS(positions); i++) {
    BtGraphLabelCand trial = c;
    trial.x = CLAMP(positions[i][0], min_x, max_x);
    trial.y = positions[i][1];
    if (i >= 2u && trial.x != positions[i][0])
      continue; /* Clamping a side position can move the label over its own node. */
    if (trial.y < min_y || trial.y > max_y)
      continue;
    if (!placement_collides(&trial, placed, placed_count, obstacles, obstacle_count))
      return trial;
    if (!found_fit) {
      fallback = trial;
      found_fit = TRUE;
    }
  }
  return fallback;
}

guint bt_graph_place_labels(BtGraphLabelCand *cands, guint cand_count,
                            const BtGraphCoreObstacle *obstacles, guint obstacle_count,
                            guint max_labels, guint max_neighbors,
                            const BtGraphLabelViewport *viewport,
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
      if (viewport != NULL && c.anchor_r > 0.0f &&
          c.anchor_x + c.anchor_r >= 0.0f && c.anchor_x - c.anchor_r <= viewport->width &&
          c.anchor_y + c.anchor_r >= 0.0f && c.anchor_y - c.anchor_r <= viewport->height) {
        out[placed] = place_primary_label(c, viewport, out, placed, obstacles, obstacle_count);
        placed++;
        continue;
      }

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

    /* Try default position first; if anchor provided, also try above / right / left. */
    gboolean fit = !placement_collides(&c, out, placed, obstacles, obstacle_count);
    if (!fit && c.anchor_r > 0.0f) {
      float alts[3][2] = {
        {floorf(c.anchor_x - c.w * 0.5f), floorf(c.anchor_y - c.anchor_r - c.h - 5.0f)},
        {floorf(c.anchor_x + c.anchor_r + 6.0f), floorf(c.anchor_y - c.h * 0.5f)},
        {floorf(c.anchor_x - c.anchor_r - c.w - 6.0f), floorf(c.anchor_y - c.h * 0.5f)},
      };
      for (int a = 0; a < 3; a++) {
        BtGraphLabelCand trial = c;
        trial.x = alts[a][0];
        trial.y = alts[a][1];
        if (!placement_collides(&trial, out, placed, obstacles, obstacle_count)) {
          c = trial;
          fit = TRUE;
          break;
        }
      }
    }
    if (!fit)
      continue;

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

static guint64 fnv1a64(const gchar *s)
{
  guint64 h = 14695981039346656037ULL;
  if (s == NULL)
    return h;
  for (const guchar *p = (const guchar *)s; *p != '\0'; p++) {
    h ^= (guint64)*p;
    h *= 1099511628211ULL;
  }
  return h;
}

static guint64 galaxy_splitmix64(guint64 *s)
{
  guint64 z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static gdouble galaxy_rand01(guint64 *s)
{
  return (gdouble)(galaxy_splitmix64(s) >> 11) * (1.0 / 9007199254740992.0);
}

static gdouble galaxy_gauss(guint64 *s)
{
  gdouble u1 = MAX(galaxy_rand01(s), 1e-12);
  gdouble u2 = galaxy_rand01(s);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

typedef struct {
  const gchar *name;
  guint count;
  gdouble anchor_r;
  gdouble anchor_phi;
  guint arm_idx;
} GalaxyFolderSlot;

static gint cmp_folder_slot_desc(const void *a, const void *b)
{
  const GalaxyFolderSlot *fa = a;
  const GalaxyFolderSlot *fb = b;
  if (fa->count != fb->count)
    return (fa->count > fb->count) ? -1 : 1;
  return g_ascii_strcasecmp(fa->name, fb->name);
}

void bt_galaxy_default_params(BtGalaxyParams *p)
{
  if (p == NULL)
    return;
  p->arms = 4;
  p->pitch_b = 0.28f;
  p->arm_spread = 0.30f;
  p->r_min = 0.12f;
  p->r_bulge = 0.18f;
  p->h_disk = 0.025f;
  p->h_bulge = 0.12f;
  p->halo_frac = 0.03f;
  p->warp = 0.06f;
  p->d_min = 0.012f;
}

void bt_galaxy_apply_diff_rot(const BtGraphVec3 *pt, gdouble gal_time, BtGraphVec3 *out)
{
  if (pt == NULL || out == NULL)
    return;
  if (fabs(gal_time) < 1e-6) {
    *out = *pt;
    return;
  }
  gdouble r = hypot((gdouble)pt->x, (gdouble)pt->z);
  gdouble ang = gal_time * (0.08 / (1.0 + r / 0.3));
  gdouble ca = cos(ang);
  gdouble sa = sin(ang);
  out->x = (float)((gdouble)pt->x * ca - (gdouble)pt->z * sa);
  out->y = pt->y;
  out->z = (float)((gdouble)pt->x * sa + (gdouble)pt->z * ca);
}

static gint cmp_guint_asc(const void *a, const void *b)
{
  guint ua = *(const guint *)a;
  guint ub = *(const guint *)b;
  return (ua > ub) - (ua < ub);
}

static guint galaxy_cell_hash(gint cx, gint cy, gint cz, guint buckets)
{
  guint64 h = (guint64)(guint32)cx * 73856093ULL ^
              (guint64)(guint32)cy * 19349663ULL ^
              (guint64)(guint32)cz * 83492791ULL;
  return (guint)(h % (guint64)buckets);
}

void bt_graph_compute_galaxy_layout(const BtGraph *g, guint arms, BtGraphVec3 *out_pts,
                                    guint max_pts)
{
  if (g == NULL || g->nodes == NULL || out_pts == NULL || max_pts == 0u)
    return;
  guint n = MIN(g->nodes->len, max_pts);
  if (n == 0u)
    return;

  BtGalaxyParams gp;
  bt_galaxy_default_params(&gp);
  guint n_arms = (arms >= 3u && arms <= 5u) ? arms : (guint)gp.arms;

  /* 0. Degree array sorted ascending for hubness percentile in [0, 1]. */
  g_autofree guint *deg_sorted = g_new(guint, n);
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    deg_sorted[i] = nd->degree;
  }
  qsort(deg_sorted, n, sizeof(guint), cmp_guint_asc);

  /* 1. Collect top-level folders & sort by descendant count desc; golden-ratio radial spread. */
  GHashTable *f_counts = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->top_folder != NULL && nd->top_folder[0] != '\0') {
      guint cur = GPOINTER_TO_UINT(g_hash_table_lookup(f_counts, nd->top_folder));
      g_hash_table_insert(f_counts, nd->top_folder, GUINT_TO_POINTER(cur + 1u));
    }
  }
  guint nf = g_hash_table_size(f_counts);
  g_autofree GalaxyFolderSlot *slots = nf > 0u ? g_new0(GalaxyFolderSlot, nf) : NULL;
  GHashTable *slot_map = g_hash_table_new(g_str_hash, g_str_equal);
  if (nf > 0u) {
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    guint si = 0u;
    g_hash_table_iter_init(&it, f_counts);
    while (g_hash_table_iter_next(&it, &k, &v) && si < nf) {
      slots[si].name = (const gchar *)k;
      slots[si].count = GPOINTER_TO_UINT(v);
      si++;
    }
    qsort(slots, nf, sizeof(GalaxyFolderSlot), cmp_folder_slot_desc);
    for (guint i = 0; i < nf; i++) {
      slots[i].arm_idx = i % n_arms;
      gdouble gr = 0.5 + (gdouble)i * 0.61803398875;
      gdouble frac = gr - floor(gr);
      gdouble r_slot = (gdouble)gp.r_min + (1.0 - (gdouble)gp.r_min) * frac;
      gdouble arm_base = ((gdouble)slots[i].arm_idx / (gdouble)n_arms) * 2.0 * M_PI;
      gdouble phi = arm_base + log(MAX(r_slot / (gdouble)gp.r_min, 1.0001)) / (gdouble)gp.pitch_b;
      slots[i].anchor_r = r_slot;
      slots[i].anchor_phi = phi;
      g_hash_table_insert(slot_map, (gpointer)slots[i].name, &slots[i]);
    }
  }

  /* 2. Place each node deterministically using splitmix64(fnv1a64(path/id)). */
  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    const gchar *key = (nd->relpath && nd->relpath[0]) ? nd->relpath : (nd->id ? nd->id : "");
    guint64 rng = fnv1a64(key);
    gdouble r = 0.22;
    gdouble phi = galaxy_rand01(&rng) * 2.0 * M_PI;

    const GalaxyFolderSlot *sl =
        (nd->top_folder != NULL) ? g_hash_table_lookup(slot_map, nd->top_folder) : NULL;

    if (nd->kind == BT_NODE_FOLDER && nd->parent_idx < 0 && sl != NULL) {
      /* Top-level folder = bright cluster star anchored on its spiral arm (z ~ 0). */
      r = sl->anchor_r;
      phi = sl->anchor_phi;
    } else if (sl != NULL) {
      gdouble arm_base = ((gdouble)sl->arm_idx / (gdouble)n_arms) * 2.0 * M_PI;
      r = CLAMP(sl->anchor_r * (1.0 + 0.18 * galaxy_gauss(&rng)), (gdouble)gp.r_min * 0.5, 1.04);
      gdouble theta_arm = arm_base + log(MAX(r / (gdouble)gp.r_min, 1.0001)) / (gdouble)gp.pitch_b;
      phi = theta_arm + (gdouble)gp.arm_spread * galaxy_gauss(&rng);
    } else if (nd->kind == BT_NODE_GHOST) {
      guint arm = (guint)(galaxy_splitmix64(&rng) % n_arms);
      r = 0.78 + galaxy_rand01(&rng) * 0.22;
      gdouble arm_base = ((gdouble)arm / (gdouble)n_arms) * 2.0 * M_PI;
      phi = arm_base + log(MAX(r / (gdouble)gp.r_min, 1.0001)) / (gdouble)gp.pitch_b +
            (gdouble)gp.arm_spread * galaxy_gauss(&rng);
    } else {
      /* Vault-root notes & sessions/tags sit near the warm inner bulge + inner arms. */
      guint arm = (guint)(galaxy_splitmix64(&rng) % n_arms);
      r = CLAMP(0.06 + fabs(galaxy_gauss(&rng)) * 0.16, 0.03, 0.42);
      gdouble arm_base = ((gdouble)arm / (gdouble)n_arms) * 2.0 * M_PI;
      phi = arm_base + log(MAX(r / (gdouble)gp.r_min, 1.0001)) / (gdouble)gp.pitch_b +
            (gdouble)gp.arm_spread * galaxy_gauss(&rng);
    }

    /* Hubness percentile in [0, 1]: hubs fall toward the warm bright core; ghosts/leaves drift out. */
    if (nd->kind != BT_NODE_FOLDER) {
      guint lo = 0, hi = n;
      while (lo < hi) {
        guint mid = lo + (hi - lo) / 2u;
        if (deg_sorted[mid] < nd->degree)
          lo = mid + 1u;
        else
          hi = mid;
      }
      gdouble hubness = (n > 1u && nd->degree > 0u) ? ((gdouble)lo / (gdouble)(n - 1u)) : 0.0;
      if (nd->degree >= 2u && hubness > 0.0)
        r = MAX(r * (1.0 - 0.6 * hubness * hubness), 0.03);
      else if (nd->kind == BT_NODE_GHOST || nd->degree <= 1u)
        r = MIN(r * 1.12, 1.08);
    }

    /* Vertical height (Y in Y-up camera): puffy bulge in center, thin disk outward,
     * 3% halo outliers (6x sigma), plus rim warp. */
    gdouble sigma_z = (gdouble)gp.h_disk + (gdouble)gp.h_bulge * exp(-r / (gdouble)gp.r_bulge);
    gdouble zy = galaxy_gauss(&rng) * sigma_z;
    if (nd->kind == BT_NODE_FOLDER && nd->parent_idx < 0) {
      zy *= 0.15; /* parent folder anchor sits near z ~ 0 */
    } else if (galaxy_rand01(&rng) < (gdouble)gp.halo_frac) {
      zy *= 6.0;
    }
    zy += (gdouble)gp.warp * (r * r) * sin(phi);
    zy = CLAMP(zy, -0.58, 0.58);

    out_pts[i].x = (float)CLAMP(r * cos(phi), -1.12, 1.12);
    out_pts[i].y = (float)zy;
    out_pts[i].z = (float)CLAMP(r * sin(phi), -1.12, 1.12);
  }

  /* 3. 3D spatial-hash collision relax (6 bounded passes) so no pair is closer than d_min. */
  const float d_min = gp.d_min * 1.05f;
  const float d_min2 = d_min * d_min;
  const float inv_cell = 1.0f / d_min;
  const guint buckets = MAX(n * 2u + 17u, 64u);
  g_autofree gint *head = g_new(gint, buckets);
  g_autofree gint *next = g_new(gint, n);

  for (int it = 0; it < 6; it++) {
    for (guint b = 0; b < buckets; b++)
      head[b] = -1;
    for (guint i = 0; i < n; i++) {
      gint cx = (gint)floorf(out_pts[i].x * inv_cell);
      gint cy = (gint)floorf(out_pts[i].y * inv_cell);
      gint cz = (gint)floorf(out_pts[i].z * inv_cell);
      guint b = galaxy_cell_hash(cx, cy, cz, buckets);
      next[i] = head[b];
      head[b] = (gint)i;
    }
    gboolean moved = FALSE;
    for (guint i = 0; i < n; i++) {
      gint cx = (gint)floorf(out_pts[i].x * inv_cell);
      gint cy = (gint)floorf(out_pts[i].y * inv_cell);
      gint cz = (gint)floorf(out_pts[i].z * inv_cell);
      for (gint ox = -1; ox <= 1; ox++) {
        for (gint oy = -1; oy <= 1; oy++) {
          for (gint oz = -1; oz <= 1; oz++) {
            guint b = galaxy_cell_hash(cx + ox, cy + oy, cz + oz, buckets);
            for (gint j = head[b]; j >= 0; j = next[j]) {
              if ((guint)j <= i)
                continue;
              float dx = out_pts[j].x - out_pts[i].x;
              float dy = out_pts[j].y - out_pts[i].y;
              float dz = out_pts[j].z - out_pts[i].z;
              float d2 = dx * dx + dy * dy + dz * dz;
              if (d2 < d_min2) {
                float d = sqrtf(d2);
                if (d < 1e-6f) {
                  float ang = (float)((i * 2654435761u + (guint)j * 31u) & 1023u) * (6.283185f / 1024.0f);
                  dx = cosf(ang) * d_min;
                  dy = 0.15f * d_min;
                  dz = sinf(ang) * d_min;
                  d = sqrtf(dx * dx + dy * dy + dz * dz);
                }
                float push = 0.55f * (d_min - d) / d;
                out_pts[i].x -= dx * push;
                out_pts[i].y -= dy * push;
                out_pts[i].z -= dz * push;
                out_pts[j].x += dx * push;
                out_pts[j].y += dy * push;
                out_pts[j].z += dz * push;
                moved = TRUE;
              }
            }
          }
        }
      }
    }
    if (!moved)
      break;
  }

  g_hash_table_destroy(slot_map);
  g_hash_table_destroy(f_counts);
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

static gboolean pick_cand_better(const BtPickCand *a, const BtPickCand *b)
{
  float dz = a->depth - b->depth;
  if (fabsf(dz) > 1e-4f)
    return dz < 0.0f; /* smaller depth = closer to camera */
  if (fabsf(a->dist2 - b->dist2) > 1e-3f)
    return a->dist2 < b->dist2; /* tie -> closest to cursor center */
  return a->node_idx < b->node_idx;
}

guint bt_graph_pick_screen(const BtScreenNode *nodes, guint count, float vp_w, float vp_h,
                           float mx, float my, BtPickCand *out, guint out_cap)
{
  if (nodes == NULL || count == 0u || vp_w <= 1.0f || vp_h <= 1.0f)
    return 0u;

  /* Uniform spatial hash grid (32px cells, 1024 buckets) for O(1)-ish picking at 5k nodes. */
  enum { CELL_PX = 32, BUCKETS = 1024 };
  gint head[BUCKETS];
  for (int i = 0; i < BUCKETS; i++)
    head[i] = -1;

  g_autofree gint *next = g_new(gint, count);
  int qcx = (int)floorf(mx / (float)CELL_PX);
  int qcy = (int)floorf(my / (float)CELL_PX);

  for (guint i = 0; i < count; i++) {
    next[i] = -1;
    if (!nodes[i].visible)
      continue;
    /* Coarse reject outside 3x3 neighborhood of cursor before inserting. */
    int cx = (int)floorf(nodes[i].sx / (float)CELL_PX);
    int cy = (int)floorf(nodes[i].sy / (float)CELL_PX);
    if (cx < qcx - 1 || cx > qcx + 1 || cy < qcy - 1 || cy > qcy + 1)
      continue;
    guint h = ((guint)(cx * 73856093) ^ (guint)(cy * 19349663)) & (BUCKETS - 1u);
    next[i] = head[h];
    head[h] = (gint)i;
  }

  BtPickCand hits[64];
  guint hit_n = 0u;
  guint total_hits = 0u;

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      int cx = qcx + dx;
      int cy = qcy + dy;
      guint h = ((guint)(cx * 73856093) ^ (guint)(cy * 19349663)) & (BUCKETS - 1u);
      for (gint idx = head[h]; idx >= 0; idx = next[idx]) {
        const BtScreenNode *sn = &nodes[idx];
        int ncx = (int)floorf(sn->sx / (float)CELL_PX);
        int ncy = (int)floorf(sn->sy / (float)CELL_PX);
        if (ncx != cx || ncy != cy)
          continue;
        float ddx = sn->sx - mx;
        float ddy = sn->sy - my;
        float d2 = ddx * ddx + ddy * ddy;
        float hr = MAX(sn->radius + 4.0f, 8.0f);
        if (d2 <= hr * hr) {
          total_hits++;
          if (hit_n < 64u) {
            hits[hit_n].node_idx = sn->node_idx;
            hits[hit_n].depth = sn->depth;
            hits[hit_n].dist2 = d2;
            hit_n++;
          }
        }
      }
    }
  }

  /* Insertion sort hits by (depth asc, dist2 asc, node_idx asc). */
  for (guint i = 1; i < hit_n; i++) {
    BtPickCand tmp = hits[i];
    guint j = i;
    while (j > 0 && pick_cand_better(&tmp, &hits[j - 1])) {
      hits[j] = hits[j - 1];
      j--;
    }
    hits[j] = tmp;
  }

  if (out != NULL && out_cap > 0u) {
    guint copy_n = MIN(hit_n, out_cap);
    for (guint i = 0; i < copy_n; i++)
      out[i] = hits[i];
  }
  return total_hits;
}

void bt_graph_apply_hover_spread(BtScreenNode *nodes, guint count, float mx, float my,
                                 float radius_px, float strength)
{
  if (nodes == NULL || count == 0u || radius_px <= 1.0f || strength <= 1e-4f)
    return;
  float r2 = radius_px * radius_px;
  for (guint i = 0; i < count; i++) {
    if (!nodes[i].visible)
      continue;
    float dx = nodes[i].sx - mx;
    float dy = nodes[i].sy - my;
    float d2 = dx * dx + dy * dy;
    if (d2 >= r2 || d2 < 1e-4f)
      continue;
    float d = sqrtf(d2);
    float t = 1.0f - (d / radius_px);
    float push = t * t * (radius_px * 0.42f) * strength;
    nodes[i].sx += (dx / d) * push;
    nodes[i].sy += (dy / d) * push;
  }
}
