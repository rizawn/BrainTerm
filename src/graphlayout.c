/* graphlayout.c — Pure-C (GTK-free) 2D force-directed graph layout engine.
 * Implements connected-component golden-angle packing, uniform spatial-grid
 * repulsion & collision, degree-weighted link springs, halo placement for
 * loose notes and size-2/3 groups, 12..40 final collision passes, and
 * component bounding-circle de-overlap. */
#include "graphlayout.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GOLDEN_ANGLE 2.39996322972865332 /* pi * (3 - sqrt(5)) */
#define DEFAULT_SEED 0x5A17C0DEu

/* --------------------------------------------------- deterministic RNG -- */

static inline uint64_t splitmix64(uint64_t *state)
{
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static inline double rng_unit(uint64_t *state)
{
  return (double)(splitmix64(state) >> 11) * (1.0 / 9007199254740992.0);
}

static uint64_t hash_node_id(const BtGraphNode *nd, guint fallback_idx, guint32 seed)
{
  uint64_t h = 1469598103934665603ULL ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ULL);
  const gchar *s = (nd != NULL && nd->id != NULL && nd->id[0] != '\0')
                       ? nd->id
                       : ((nd != NULL && nd->relpath != NULL) ? nd->relpath : NULL);
  if (s != NULL) {
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
      h ^= (uint64_t)(*p);
      h *= 1099511628211ULL;
    }
  } else {
    h ^= (uint64_t)fallback_idx * 0xBF58476D1CE4E5B9ULL;
    h *= 1099511628211ULL;
  }
  return h;
}

/* ------------------------------------------------------- radius model -- */

float bt_graph_node_radius_for_degree(BtNodeKind kind, guint vis_degree)
{
  float r = 2.5f + 1.3f * sqrtf((float)vis_degree);
  if (r < 2.5f)
    r = 2.5f;
  if (r > 10.0f)
    r = 10.0f;
  if (kind == BT_NODE_TAG)
    r *= 0.8f;
  else if (kind == BT_NODE_GHOST)
    r *= 0.9f;
  return r;
}

float bt_graph_node_radius(const BtGraph *g, guint node_index)
{
  if (g == NULL || g->nodes == NULL || node_index >= g->nodes->len)
    return 2.5f;
  const BtGraphNode *nd = g_ptr_array_index(g->nodes, node_index);
  return bt_graph_node_radius_for_degree(nd->kind, nd->degree);
}

void bt_graph_layout_default_opts(BtGraphLayoutOpts *o)
{
  if (o == NULL)
    return;
  o->seed = DEFAULT_SEED;
  o->max_iterations = 300u;
  o->link_distance = 30.0f;
  o->gravity = 0.03f;
  o->collision_pad = 1.5f;
  o->warm_start = FALSE;
  o->time_cap_ms = 0.0;
}

/* ------------------------------------------------------- spatial grid -- */

typedef struct {
  gint *head;
  gint *next;
  guint buckets;
  guint cap_nodes;
  float inv_cell;
} SpatialGrid;

static void grid_init(SpatialGrid *sg, guint node_cap, float cell_size)
{
  sg->cap_nodes = MAX(node_cap, 1u);
  sg->buckets = MAX(node_cap * 2u + 17u, 64u);
  sg->head = g_new(gint, sg->buckets);
  sg->next = g_new(gint, sg->cap_nodes);
  sg->inv_cell = (cell_size > 1e-4f) ? (1.0f / cell_size) : 1.0f;
}

static void grid_free(SpatialGrid *sg)
{
  g_clear_pointer(&sg->head, g_free);
  g_clear_pointer(&sg->next, g_free);
  sg->buckets = 0;
  sg->cap_nodes = 0;
}

static inline guint grid_hash(gint cx, gint cy, guint buckets)
{
  uint64_t h = (uint64_t)(uint32_t)cx * 73856093ULL ^ (uint64_t)(uint32_t)cy * 19349663ULL;
  return (guint)(h % (uint64_t)buckets);
}

static void grid_clear(SpatialGrid *sg)
{
  for (guint i = 0; i < sg->buckets; i++)
    sg->head[i] = -1;
}

static inline void grid_insert(SpatialGrid *sg, guint idx, float x, float y)
{
  gint cx = (gint)floorf(x * sg->inv_cell);
  gint cy = (gint)floorf(y * sg->inv_cell);
  guint b = grid_hash(cx, cy, sg->buckets);
  sg->next[idx] = sg->head[b];
  sg->head[b] = (gint)idx;
}

/* ----------------------------------------------------- internal state -- */

typedef struct {
  guint a;
  guint b;
  float k; /* 1 / min(deg_a, deg_b) */
} SimEdge;

typedef struct {
  guint *members;
  guint count;
  float cx, cy;
  float radius;
  gboolean is_halo;
} CompInfo;

struct BtGraphLayoutState {
  BtGraph *g;
  BtGraphLayoutOpts opts;

  guint n_total;      /* g->nodes->len */
  guint n_vis;        /* number of visible nodes */
  guint *vis_to_orig; /* [n_vis] -> index in g->nodes */
  gint *orig_to_vis;  /* [n_total] -> index in [0..n_vis-1] or -1 */

  float *x;
  float *y;
  float *x0; /* initial safe positions for NaN fallback */
  float *y0;
  float *vx;
  float *vy;
  float *rad;
  guint *deg;
  guint8 *pinned;
  guint8 *is_halo;
  gint *comp_id;

  SimEdge *edges;
  guint n_edges;

  CompInfo *comps;
  guint n_comps;
  guint n_main_comps;

  SpatialGrid rep_grid;
  SpatialGrid col_grid;

  float alpha;
  float alpha_decay;
  float alpha_min;
  float vel_decay;
  guint iter_done;
  guint quiet_streak;
  guint nan_resets;
  guint remaining_overlaps;
  gboolean finished;
};

typedef struct {
  guint idx;
  guint deg;
} DegSortKey;

static gint cmp_deg_desc(const void *a, const void *b)
{
  const DegSortKey *da = a;
  const DegSortKey *db = b;
  if (da->deg != db->deg)
    return (da->deg > db->deg) ? -1 : 1;
  return (da->idx > db->idx) - (da->idx < db->idx);
}

static gint cmp_comp_desc(const void *a, const void *b)
{
  const CompInfo *ca = a;
  const CompInfo *cb = b;
  if (ca->count != cb->count)
    return (ca->count > cb->count) ? -1 : 1;
  guint ma = (ca->count > 0) ? ca->members[0] : 0u;
  guint mb = (cb->count > 0) ? cb->members[0] : 0u;
  return (ma > mb) - (ma < mb);
}

static uint64_t edge_pair_key(guint a, guint b)
{
  guint lo = a < b ? a : b;
  guint hi = a < b ? b : a;
  return ((uint64_t)lo << 32) | (uint64_t)hi;
}

static gint cmp_u64_asc(const void *a, const void *b)
{
  uint64_t ua = *(const uint64_t *)a;
  uint64_t ub = *(const uint64_t *)b;
  return (ua > ub) - (ua < ub);
}

/* ------------------------------------------- components & initial pos -- */

static void compute_components(BtGraphLayoutState *s)
{
  guint n = s->n_vis;
  if (n == 0u)
    return;

  /* CSR adjacency over deduplicated visible edges. */
  g_autofree guint *adj_off = g_new0(guint, n + 1u);
  for (guint e = 0; e < s->n_edges; e++) {
    adj_off[s->edges[e].a + 1u]++;
    adj_off[s->edges[e].b + 1u]++;
  }
  for (guint i = 0; i < n; i++)
    adj_off[i + 1u] += adj_off[i];

  g_autofree guint *adj = g_new(guint, MAX(adj_off[n], 1u));
  g_autofree guint *cursor = g_memdup2(adj_off, (n + 1u) * sizeof(guint));
  for (guint e = 0; e < s->n_edges; e++) {
    guint a = s->edges[e].a;
    guint b = s->edges[e].b;
    adj[cursor[a]++] = b;
    adj[cursor[b]++] = a;
  }

  g_autofree guint8 *visited = g_new0(guint8, n);
  g_autofree guint *queue = g_new(guint, n);
  GArray *carr = g_array_new(FALSE, FALSE, sizeof(CompInfo));

  for (guint i = 0; i < n; i++) {
    if (visited[i])
      continue;
    guint qh = 0, qt = 0;
    queue[qt++] = i;
    visited[i] = 1u;
    while (qh < qt) {
      guint u = queue[qh++];
      for (guint k = adj_off[u]; k < adj_off[u + 1u]; k++) {
        guint v = adj[k];
        if (!visited[v]) {
          visited[v] = 1u;
          queue[qt++] = v;
        }
      }
    }
    CompInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.count = qt;
    ci.members = g_new(guint, qt);
    memcpy(ci.members, queue, qt * sizeof(guint));
    /* Sort members by degree descending so highest-degree hub sits at center. */
    g_autofree DegSortKey *dkeys = g_new(DegSortKey, qt);
    for (guint k = 0; k < qt; k++) {
      dkeys[k].idx = ci.members[k];
      dkeys[k].deg = s->deg[ci.members[k]];
    }
    qsort(dkeys, qt, sizeof(DegSortKey), cmp_deg_desc);
    for (guint k = 0; k < qt; k++)
      ci.members[k] = dkeys[k].idx;
    ci.is_halo = (qt <= 3u);
    g_array_append_val(carr, ci);
  }

  s->n_comps = carr->len;
  s->comps = (CompInfo *)g_array_free(carr, FALSE);
  qsort(s->comps, s->n_comps, sizeof(CompInfo), cmp_comp_desc);

  s->n_main_comps = 0u;
  for (guint c = 0; c < s->n_comps; c++) {
    if (!s->comps[c].is_halo)
      s->n_main_comps++;
    for (guint k = 0; k < s->comps[c].count; k++) {
      guint u = s->comps[c].members[k];
      s->comp_id[u] = (gint)c;
      s->is_halo[u] = s->comps[c].is_halo ? 1u : 0u;
    }
  }
}

static void place_halo_components(BtGraphLayoutState *s, float r_main)
{
  guint halo_comps = s->n_comps - s->n_main_comps;
  if (halo_comps == 0u)
    return;

  float base_r = MAX(r_main, 36.0f);
  /* Ensure halo circumference has enough area for large numbers of loose notes. */
  guint halo_nodes = 0u;
  for (guint c = s->n_main_comps; c < s->n_comps; c++)
    halo_nodes += s->comps[c].count;
  float min_ring_r = sqrtf((float)halo_nodes * 180.0f / (float)M_PI);
  if (base_r < min_ring_r * 0.78f)
    base_r = min_ring_r * 0.78f;

  float r_lo = base_r * 1.12f;
  float r_hi = base_r * 1.45f;
  float band = MAX(r_hi - r_lo, 28.0f);

  guint h_idx = 0u;
  for (guint c = s->n_main_comps; c < s->n_comps; c++) {
    CompInfo *ci = &s->comps[c];
    const BtGraphNode *nd0 = g_ptr_array_index(s->g->nodes, s->vis_to_orig[ci->members[0]]);
    uint64_t rng = hash_node_id(nd0, ci->members[0], s->opts.seed ^ 0x48A10u);

    double angle = (double)h_idx * GOLDEN_ANGLE + (rng_unit(&rng) - 0.5) * 0.08;
    double u_rad = rng_unit(&rng);
    float r = r_lo + (float)u_rad * band;
    float cx = (float)(cos(angle) * (double)r);
    float cy = (float)(sin(angle) * (double)r);

    ci->cx = cx;
    ci->cy = cy;

    if (ci->count == 1u) {
      guint u = ci->members[0];
      if (!s->pinned[u]) {
        s->x[u] = cx;
        s->y[u] = cy;
      }
      ci->radius = s->rad[u];
    } else if (ci->count == 2u) {
      guint u0 = ci->members[0];
      guint u1 = ci->members[1];
      float half = 0.5f * (s->rad[u0] + s->rad[u1] + s->opts.collision_pad + 8.0f);
      double tang = angle + M_PI * 0.5;
      float dx = (float)(cos(tang) * (double)half);
      float dy = (float)(sin(tang) * (double)half);
      if (!s->pinned[u0]) { s->x[u0] = cx - dx; s->y[u0] = cy - dy; }
      if (!s->pinned[u1]) { s->x[u1] = cx + dx; s->y[u1] = cy + dy; }
      ci->radius = half + MAX(s->rad[u0], s->rad[u1]);
    } else {
      /* 3-node tiny group: small equilateral triangle centered on halo slot. */
      float max_r = 0.0f;
      for (guint k = 0; k < ci->count; k++)
        if (s->rad[ci->members[k]] > max_r)
          max_r = s->rad[ci->members[k]];
      float tri_r = (max_r * 2.0f + s->opts.collision_pad + 6.0f) * 0.65f;
      for (guint k = 0; k < ci->count; k++) {
        guint u = ci->members[k];
        double a = angle + (double)k * (2.0 * M_PI / (double)ci->count);
        if (!s->pinned[u]) {
          s->x[u] = cx + (float)(cos(a) * (double)tri_r);
          s->y[u] = cy + (float)(sin(a) * (double)tri_r);
        }
      }
      ci->radius = tri_r + max_r;
    }
    h_idx++;
  }
}

static void initial_placement(BtGraphLayoutState *s)
{
  float L0 = s->opts.link_distance;

  if (s->opts.warm_start) {
    /* Keep existing finite positions; place newly visible / non-finite nodes near neighbors. */
    g_autofree guint8 *has_pos = g_new0(guint8, s->n_vis);
    for (guint i = 0; i < s->n_vis; i++) {
      const BtGraphNode *nd = g_ptr_array_index(s->g->nodes, s->vis_to_orig[i]);
      if (isfinite(nd->x) && isfinite(nd->y) && (fabs(nd->x) > 1e-4 || fabs(nd->y) > 1e-4)) {
        s->x[i] = (float)nd->x;
        s->y[i] = (float)nd->y;
        has_pos[i] = 1u;
      }
    }
    /* Accumulate neighbor averages for newly visible nodes. */
    g_autofree float *sum_x = g_new0(float, s->n_vis);
    g_autofree float *sum_y = g_new0(float, s->n_vis);
    g_autofree guint *cnt = g_new0(guint, s->n_vis);
    for (guint e = 0; e < s->n_edges; e++) {
      guint a = s->edges[e].a, b = s->edges[e].b;
      if (has_pos[a] && !has_pos[b]) { sum_x[b] += s->x[a]; sum_y[b] += s->y[a]; cnt[b]++; }
      if (has_pos[b] && !has_pos[a]) { sum_x[a] += s->x[b]; sum_y[a] += s->y[b]; cnt[a]++; }
    }
    float r_main = 40.0f;
    for (guint i = 0; i < s->n_vis; i++) {
      if (has_pos[i] && !s->is_halo[i]) {
        float d = hypotf(s->x[i], s->y[i]) + s->rad[i];
        if (d > r_main) r_main = d;
      }
    }
    for (guint i = 0; i < s->n_vis; i++) {
      if (has_pos[i])
        continue;
      const BtGraphNode *nd = g_ptr_array_index(s->g->nodes, s->vis_to_orig[i]);
      uint64_t rng = hash_node_id(nd, i, s->opts.seed);
      if (cnt[i] > 0u) {
        s->x[i] = sum_x[i] / (float)cnt[i] + (float)(rng_unit(&rng) - 0.5) * 14.0f;
        s->y[i] = sum_y[i] / (float)cnt[i] + (float)(rng_unit(&rng) - 0.5) * 14.0f;
      } else {
        double a = (double)i * GOLDEN_ANGLE;
        float r = (r_main + 24.0f) * (1.12f + (float)rng_unit(&rng) * 0.33f);
        s->x[i] = (float)(cos(a) * (double)r);
        s->y[i] = (float)(sin(a) * (double)r);
      }
    }
    memcpy(s->x0, s->x, s->n_vis * sizeof(float));
    memcpy(s->y0, s->y, s->n_vis * sizeof(float));
    return;
  }

  /* Fresh deterministic placement: pack main components (size >= 4) on golden-angle spiral. */
  const float comp_margin = 24.0f;
  float r_main = 0.0f;

  for (guint c = 0; c < s->n_main_comps; c++) {
    CompInfo *ci = &s->comps[c];
    float r_max = 2.5f;
    for (guint k = 0; k < ci->count; k++)
      if (s->rad[ci->members[k]] > r_max)
        r_max = s->rad[ci->members[k]];
    ci->radius = L0 * sqrtf((float)ci->count) * 0.9f + r_max;

    if (c == 0u) {
      ci->cx = 0.0f;
      ci->cy = 0.0f;
    } else {
      /* Walk golden-angle spiral outward until disc (radius + 24) clears all prior components. */
      float placed_x = 0.0f, placed_y = 0.0f;
      float step_r = 18.0f;
      for (guint step = 1u; step < 4096u; step++) {
        double ang = (double)step * GOLDEN_ANGLE;
        float dist = step_r * sqrtf((float)step);
        float tx = (float)(cos(ang) * (double)dist);
        float ty = (float)(sin(ang) * (double)dist);
        gboolean ok = TRUE;
        for (guint prev = 0; prev < c; prev++) {
          float dx = tx - s->comps[prev].cx;
          float dy = ty - s->comps[prev].cy;
          float min_d = ci->radius + s->comps[prev].radius + comp_margin;
          if (dx * dx + dy * dy < min_d * min_d) {
            ok = FALSE;
            break;
          }
        }
        if (ok) {
          placed_x = tx;
          placed_y = ty;
          break;
        }
      }
      ci->cx = placed_x;
      ci->cy = placed_y;
    }

    float outer = hypotf(ci->cx, ci->cy) + ci->radius;
    if (outer > r_main)
      r_main = outer;

    /* Phyllotaxis spiral inside component, highest-degree node at center. */
    float spacing = MAX(L0 * 0.42f, 10.0f);
    for (guint k = 0; k < ci->count; k++) {
      guint u = ci->members[k];
      const BtGraphNode *nd = g_ptr_array_index(s->g->nodes, s->vis_to_orig[u]);
      uint64_t rng = hash_node_id(nd, u, s->opts.seed);
      float jx = (float)(rng_unit(&rng) - 0.5) * 1.2f;
      float jy = (float)(rng_unit(&rng) - 0.5) * 1.2f;
      if (k == 0u) {
        s->x[u] = ci->cx + jx;
        s->y[u] = ci->cy + jy;
      } else {
        double ang = (double)k * GOLDEN_ANGLE;
        float rr = spacing * sqrtf((float)k);
        s->x[u] = ci->cx + (float)(cos(ang) * (double)rr) + jx;
        s->y[u] = ci->cy + (float)(sin(ang) * (double)rr) + jy;
      }
    }
  }

  place_halo_components(s, r_main + 24.0f);
  memcpy(s->x0, s->x, s->n_vis * sizeof(float));
  memcpy(s->y0, s->y, s->n_vis * sizeof(float));
}

/* ------------------------------------------- collision & de-overlap -- */

static guint run_collision_pass(BtGraphLayoutState *s, gboolean include_halo, float strength)
{
  guint n = s->n_vis;
  if (n < 2u)
    return 0u;

  grid_clear(&s->col_grid);
  for (guint i = 0; i < n; i++) {
    if (!include_halo && s->is_halo[i])
      continue;
    grid_insert(&s->col_grid, i, s->x[i], s->y[i]);
  }

  guint overlaps = 0u;
  float pad = s->opts.collision_pad;
  for (guint i = 0; i < n; i++) {
    if (!include_halo && s->is_halo[i])
      continue;
    gint cx = (gint)floorf(s->x[i] * s->col_grid.inv_cell);
    gint cy = (gint)floorf(s->y[i] * s->col_grid.inv_cell);
    for (gint ox = -1; ox <= 1; ox++) {
      for (gint oy = -1; oy <= 1; oy++) {
        guint b = grid_hash(cx + ox, cy + oy, s->col_grid.buckets);
        for (gint j = s->col_grid.head[b]; j >= 0; j = s->col_grid.next[j]) {
          if ((guint)j <= i)
            continue;
          float dx = s->x[j] - s->x[i];
          float dy = s->y[j] - s->y[i];
          float min_d = s->rad[i] + s->rad[j] + pad;
          float d2 = dx * dx + dy * dy;
          if (d2 < min_d * min_d) {
            overlaps++;
            float d = sqrtf(d2);
            float nx, ny;
            if (d < 1e-4f) {
              double ang = (double)((i * 2654435761u + (guint)j * 31u) & 1023u) *
                           (2.0 * M_PI / 1024.0);
              nx = (float)cos(ang);
              ny = (float)sin(ang);
              d = 0.0f;
            } else {
              nx = dx / d;
              ny = dy / d;
            }
            float push = (min_d - d) * strength;
            gboolean pi = s->pinned[i] != 0u;
            gboolean pj = s->pinned[j] != 0u;
            if (!pi && !pj) {
              s->x[i] -= nx * push * 0.5f;
              s->y[i] -= ny * push * 0.5f;
              s->x[j] += nx * push * 0.5f;
              s->y[j] += ny * push * 0.5f;
            } else if (!pi && pj) {
              s->x[i] -= nx * push;
              s->y[i] -= ny * push;
            } else if (pi && !pj) {
              s->x[j] += nx * push;
              s->y[j] += ny * push;
            }
          }
        }
      }
    }
  }
  return overlaps;
}

static guint count_remaining_overlaps(const BtGraphLayoutState *s, float *worst_ratio_out)
{
  guint n = s->n_vis;
  float pad = s->opts.collision_pad;
  float worst = 1.0f;
  guint ov = 0u;

  /* Use col_grid (non-const cast for scratch reuse is safe inside finish). */
  SpatialGrid *cg = (SpatialGrid *)&s->col_grid;
  grid_clear(cg);
  for (guint i = 0; i < n; i++)
    grid_insert(cg, i, s->x[i], s->y[i]);

  for (guint i = 0; i < n; i++) {
    gint cx = (gint)floorf(s->x[i] * cg->inv_cell);
    gint cy = (gint)floorf(s->y[i] * cg->inv_cell);
    for (gint ox = -1; ox <= 1; ox++) {
      for (gint oy = -1; oy <= 1; oy++) {
        guint b = grid_hash(cx + ox, cy + oy, cg->buckets);
        for (gint j = cg->head[b]; j >= 0; j = cg->next[j]) {
          if ((guint)j <= i)
            continue;
          float dx = s->x[j] - s->x[i];
          float dy = s->y[j] - s->y[i];
          float min_d = s->rad[i] + s->rad[j] + pad;
          float d2 = dx * dx + dy * dy;
          if (d2 < min_d * min_d - 1e-3f) {
            ov++;
            float d = MAX(sqrtf(d2), 0.1f);
            float ratio = min_d / d;
            if (ratio > worst)
              worst = ratio;
          }
        }
      }
    }
  }
  if (worst_ratio_out != NULL)
    *worst_ratio_out = worst;
  return ov;
}

static void recompute_component_circles(BtGraphLayoutState *s)
{
  for (guint c = 0; c < s->n_comps; c++) {
    CompInfo *ci = &s->comps[c];
    if (ci->count == 0u)
      continue;
    double sx = 0.0, sy = 0.0;
    for (guint k = 0; k < ci->count; k++) {
      sx += (double)s->x[ci->members[k]];
      sy += (double)s->y[ci->members[k]];
    }
    ci->cx = (float)(sx / (double)ci->count);
    ci->cy = (float)(sy / (double)ci->count);
    float max_r = 0.0f;
    for (guint k = 0; k < ci->count; k++) {
      guint u = ci->members[k];
      float d = hypotf(s->x[u] - ci->cx, s->y[u] - ci->cy) + s->rad[u];
      if (d > max_r)
        max_r = d;
    }
    ci->radius = max_r;
  }
}

static void deoverlap_components(BtGraphLayoutState *s)
{
  const float margin = 8.0f;
  for (int pass = 0; pass < 16; pass++) {
    recompute_component_circles(s);
    gboolean moved = FALSE;
    for (guint i = 0; i < s->n_comps; i++) {
      if (s->comps[i].count < 2u)
        continue;
      for (guint j = i + 1u; j < s->n_comps; j++) {
        if (s->comps[j].count < 2u)
          continue;
        float dx = s->comps[j].cx - s->comps[i].cx;
        float dy = s->comps[j].cy - s->comps[i].cy;
        float min_d = s->comps[i].radius + s->comps[j].radius + margin;
        float d2 = dx * dx + dy * dy;
        if (d2 < min_d * min_d) {
          float d = sqrtf(d2);
          float nx, ny;
          if (d < 1e-4f) {
            double ang = (double)(j * 17u + i * 7u) * GOLDEN_ANGLE;
            nx = (float)cos(ang);
            ny = (float)sin(ang);
            d = 0.0f;
          } else {
            nx = dx / d;
            ny = dy / d;
          }
          float shift = (min_d - d) + 1.0f;
          /* Move the smaller component (j, since comps are sorted desc by size) outward. */
          CompInfo *smaller = &s->comps[j];
          for (guint k = 0; k < smaller->count; k++) {
            guint u = smaller->members[k];
            if (!s->pinned[u]) {
              s->x[u] += nx * shift;
              s->y[u] += ny * shift;
            }
          }
          smaller->cx += nx * shift;
          smaller->cy += ny * shift;
          moved = TRUE;
        }
      }
    }
    if (!moved)
      break;
  }
}

/* --------------------------------------------------- simulation step -- */

static void publish_to_graph(BtGraphLayoutState *s)
{
  for (guint i = 0; i < s->n_vis; i++) {
    BtGraphNode *nd = g_ptr_array_index(s->g->nodes, s->vis_to_orig[i]);
    nd->x = (gdouble)s->x[i];
    nd->y = (gdouble)s->y[i];
    nd->radius = (gdouble)s->rad[i];
  }
}

static gboolean step_once(BtGraphLayoutState *s)
{
  guint n = s->n_vis;
  if (n < 2u || s->alpha < s->alpha_min || s->iter_done >= s->opts.max_iterations)
    return FALSE;

  float alpha = s->alpha;
  float L = s->opts.link_distance;
  float grav = s->opts.gravity;

  /* 1. Link spring forces (only between non-halo nodes; halo size-2/3 keep crisp shape). */
  for (guint e = 0; e < s->n_edges; e++) {
    guint a = s->edges[e].a;
    guint b = s->edges[e].b;
    if (s->is_halo[a] || s->is_halo[b])
      continue;
    float dx = s->x[b] - s->x[a];
    float dy = s->y[b] - s->y[a];
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 1e-4f) {
      dx = 0.5f;
      dy = 0.0f;
      dist = 0.5f;
    }
    float target_l = L + (s->rad[a] + s->rad[b]) * 0.65f;
    float diff = (dist - target_l) / dist;
    float f = diff * s->edges[e].k * 0.55f * alpha;
    float fx = dx * f;
    float fy = dy * f;
    if (!s->pinned[a]) { s->vx[a] += fx; s->vy[a] += fy; }
    if (!s->pinned[b]) { s->vx[b] -= fx; s->vy[b] -= fy; }
  }

  /* 2. Spatial-grid node repulsion over main-component nodes (cutoff = 4 * L, 3x3 cell stencil). */
  float r_cut = 4.5f * L;
  float r_cut2 = r_cut * r_cut;
  float rep_strength = L * L * 1.25f;
  float soft2 = 24.0f;

  grid_clear(&s->rep_grid);
  for (guint i = 0; i < n; i++) {
    if (!s->is_halo[i])
      grid_insert(&s->rep_grid, i, s->x[i], s->y[i]);
  }

  for (guint i = 0; i < n; i++) {
    if (s->is_halo[i])
      continue;
    gint cx = (gint)floorf(s->x[i] * s->rep_grid.inv_cell);
    gint cy = (gint)floorf(s->y[i] * s->rep_grid.inv_cell);
    for (gint ox = -1; ox <= 1; ox++) {
      for (gint oy = -1; oy <= 1; oy++) {
        guint b = grid_hash(cx + ox, cy + oy, s->rep_grid.buckets);
        for (gint j = s->rep_grid.head[b]; j >= 0; j = s->rep_grid.next[j]) {
          if ((guint)j <= i)
            continue;
          float dx = s->x[i] - s->x[j];
          float dy = s->y[i] - s->y[j];
          float d2 = dx * dx + dy * dy;
          if (d2 >= r_cut2)
            continue;
          float d = sqrtf(d2);
          float nx, ny;
          if (d < 1e-4f) {
            double ang = (double)((i * 2654435761u + (guint)j * 17u) & 1023u) *
                         (2.0 * M_PI / 1024.0);
            nx = (float)cos(ang);
            ny = (float)sin(ang);
            d = 0.1f;
            d2 = 0.01f;
          } else {
            nx = dx / d;
            ny = dy / d;
          }
          /* Smooth falloff to 0 at r_cut so unlinked pairs at ~2*L barely drift. */
          float falloff = 1.0f - (d / r_cut);
          float f = (rep_strength * falloff * falloff) / (d2 + soft2) * alpha;
          float fx = nx * f;
          float fy = ny * f;
          if (!s->pinned[i]) { s->vx[i] += fx; s->vy[i] += fy; }
          if (!s->pinned[j]) { s->vx[j] -= fx; s->vy[j] -= fy; }
        }
      }
    }
  }

  /* 3. Weak center gravity + velocity integration with damping & 40px safety clamp. */
  float max_disp = 0.0f;
  for (guint i = 0; i < n; i++) {
    if (s->is_halo[i] || s->pinned[i]) {
      s->vx[i] = 0.0f;
      s->vy[i] = 0.0f;
      continue;
    }
    s->vx[i] += (-s->x[i]) * grav * alpha;
    s->vy[i] += (-s->y[i]) * grav * alpha;

    s->vx[i] *= s->vel_decay;
    s->vy[i] *= s->vel_decay;

    float dx = s->vx[i];
    float dy = s->vy[i];
    float disp = sqrtf(dx * dx + dy * dy);
    if (disp > 40.0f) {
      float sc = 40.0f / disp;
      dx *= sc;
      dy *= sc;
      s->vx[i] = dx;
      s->vy[i] = dy;
      disp = 40.0f;
    }
    s->x[i] += dx;
    s->y[i] += dy;

    if (!isfinite(s->x[i]) || !isfinite(s->y[i])) {
      s->x[i] = s->x0[i];
      s->y[i] = s->y0[i];
      s->vx[i] = 0.0f;
      s->vy[i] = 0.0f;
      s->nan_resets++;
    } else if (disp > max_disp) {
      max_disp = disp;
    }
  }

  /* 4. Per-iteration disc collision on main nodes. */
  run_collision_pass(s, FALSE, 0.7f);

  s->iter_done++;
  s->alpha *= s->alpha_decay;

  if (max_disp < 0.02f) {
    s->quiet_streak++;
    if (s->quiet_streak >= 10u)
      return FALSE;
  } else {
    s->quiet_streak = 0u;
  }
  return (s->alpha >= s->alpha_min && s->iter_done < s->opts.max_iterations);
}

/* --------------------------------------------------------- public API -- */

BtGraphLayoutState *bt_graph_layout_begin(BtGraph *g, const BtGraphLayoutOpts *o)
{
  if (g == NULL || g->nodes == NULL)
    return NULL;

  BtGraphLayoutState *s = g_new0(BtGraphLayoutState, 1);
  s->g = g;
  if (o != NULL)
    s->opts = *o;
  else
    bt_graph_layout_default_opts(&s->opts);

  if (s->opts.seed == 0u)
    s->opts.seed = DEFAULT_SEED;
  if (s->opts.max_iterations == 0u)
    s->opts.max_iterations = 300u;
  if (s->opts.link_distance < 8.0f)
    s->opts.link_distance = 30.0f;
  if (s->opts.collision_pad < 0.0f)
    s->opts.collision_pad = 1.5f;

  s->n_total = g->nodes->len;
  s->orig_to_vis = g_new(gint, MAX(s->n_total, 1u));
  for (guint i = 0; i < s->n_total; i++)
    s->orig_to_vis[i] = -1;

  guint vis_cnt = 0u;
  for (guint i = 0; i < s->n_total; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->visible)
      vis_cnt++;
  }
  s->n_vis = vis_cnt;
  if (vis_cnt == 0u) {
    s->finished = TRUE;
    return s;
  }

  s->vis_to_orig = g_new(guint, vis_cnt);
  guint vi = 0u;
  for (guint i = 0; i < s->n_total; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->visible) {
      s->vis_to_orig[vi] = i;
      s->orig_to_vis[i] = (gint)vi;
      vi++;
    }
  }

  s->x = g_new0(float, vis_cnt);
  s->y = g_new0(float, vis_cnt);
  s->x0 = g_new0(float, vis_cnt);
  s->y0 = g_new0(float, vis_cnt);
  s->vx = g_new0(float, vis_cnt);
  s->vy = g_new0(float, vis_cnt);
  s->rad = g_new0(float, vis_cnt);
  s->deg = g_new0(guint, vis_cnt);
  s->pinned = g_new0(guint8, vis_cnt);
  s->is_halo = g_new0(guint8, vis_cnt);
  s->comp_id = g_new(gint, vis_cnt);
  for (guint i = 0; i < vis_cnt; i++)
    s->comp_id[i] = -1;

  /* Deduplicate visible, non-self-loop edges & compute visible degree. */
  guint raw_e = (g->edges != NULL) ? g->edges->len : 0u;
  if (raw_e > 0u) {
    g_autofree uint64_t *keys = g_new(uint64_t, raw_e);
    guint nk = 0u;
    for (guint e = 0; e < raw_e; e++) {
      const BtGraphEdge *ed = &g_array_index(g->edges, BtGraphEdge, e);
      if (ed->src_idx >= s->n_total || ed->dst_idx >= s->n_total || ed->src_idx == ed->dst_idx)
        continue;
      gint va = s->orig_to_vis[ed->src_idx];
      gint vb = s->orig_to_vis[ed->dst_idx];
      if (va < 0 || vb < 0)
        continue;
      keys[nk++] = edge_pair_key((guint)va, (guint)vb);
    }
    if (nk > 0u) {
      qsort(keys, nk, sizeof(uint64_t), cmp_u64_asc);
      s->edges = g_new(SimEdge, nk);
      guint ne = 0u;
      for (guint k = 0; k < nk; k++) {
        if (k > 0u && keys[k] == keys[k - 1u])
          continue;
        guint a = (guint)(keys[k] >> 32);
        guint b = (guint)(keys[k] & 0xFFFFFFFFu);
        s->edges[ne].a = a;
        s->edges[ne].b = b;
        s->edges[ne].k = 1.0f;
        s->deg[a]++;
        s->deg[b]++;
        ne++;
      }
      s->n_edges = ne;
      for (guint e = 0; e < ne; e++) {
        guint md = MIN(s->deg[s->edges[e].a], s->deg[s->edges[e].b]);
        s->edges[e].k = 1.0f / (float)MAX(md, 1u);
      }
    }
  }

  /* Compute shared node radii from visible degree. */
  for (guint i = 0; i < vis_cnt; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, s->vis_to_orig[i]);
    s->rad[i] = bt_graph_node_radius_for_degree(nd->kind, s->deg[i]);
  }

  grid_init(&s->rep_grid, vis_cnt, MAX(s->opts.link_distance * 4.5f, 48.0f));
  grid_init(&s->col_grid, vis_cnt, 24.0f);

  compute_components(s);
  initial_placement(s);

  s->alpha = s->opts.warm_start ? 0.30f : 1.0f;
  s->alpha_min = 0.001f;
  s->alpha_decay = powf(s->alpha_min / s->alpha, 1.0f / (float)s->opts.max_iterations);
  s->vel_decay = 0.60f;
  publish_to_graph(s);
  return s;
}

gboolean bt_graph_layout_step(BtGraphLayoutState *s, guint n, gdouble budget_ms)
{
  if (s == NULL || s->finished || s->n_vis < 2u || s->n_main_comps == 0u)
    return FALSE;

  gint64 t0 = (budget_ms > 0.0) ? g_get_monotonic_time() : 0;
  guint steps = (n > 0u) ? n : 1u;
  gboolean moving = TRUE;

  for (guint k = 0; k < steps; k++) {
    moving = step_once(s);
    if (!moving)
      break;
    if (budget_ms > 0.0) {
      gdouble elapsed = (gdouble)(g_get_monotonic_time() - t0) / 1000.0;
      if (elapsed >= budget_ms)
        break;
    }
  }

  publish_to_graph(s);
  return moving;
}

void bt_graph_layout_finish(BtGraphLayoutState *s)
{
  if (s == NULL || s->finished)
    return;
  s->finished = TRUE;

  if (s->n_vis == 0u)
    return;

  /* 1. Re-center main components and re-seat halo ring around settled main cluster. */
  if (s->n_main_comps > 0u && !s->opts.warm_start) {
    /* Shift main nodes so their centroid is near origin. */
    double mx = 0.0, my = 0.0;
    guint mc = 0u;
    for (guint i = 0; i < s->n_vis; i++) {
      if (!s->is_halo[i]) {
        mx += (double)s->x[i];
        my += (double)s->y[i];
        mc++;
      }
    }
    if (mc > 0u) {
      float cx = (float)(mx / (double)mc);
      float cy = (float)(my / (double)mc);
      for (guint i = 0; i < s->n_vis; i++) {
        if (!s->is_halo[i] && !s->pinned[i]) {
          s->x[i] -= cx;
          s->y[i] -= cy;
        }
      }
    }
    /* De-overlap main components first so r_main is accurate. */
    deoverlap_components(s);

    float r_main = 0.0f;
    for (guint i = 0; i < s->n_vis; i++) {
      if (!s->is_halo[i]) {
        float d = hypotf(s->x[i], s->y[i]) + s->rad[i];
        if (d > r_main)
          r_main = d;
      }
    }
    place_halo_components(s, r_main + 24.0f);
  } else {
    deoverlap_components(s);
  }

  /* 2. Final collision relaxation: at least 12 passes, up to 40 until 0 overlaps. */
  for (int pass = 0; pass < 40; pass++) {
    guint ov = run_collision_pass(s, TRUE, 1.0f);
    if (pass >= 11 && ov == 0u)
      break;
  }

  /* 3. Final component bounding-circle de-overlap (size >= 2, 8 px margin), then light settle. */
  deoverlap_components(s);
  for (int pass = 0; pass < 16; pass++) {
    guint ov = run_collision_pass(s, TRUE, 1.0f);
    if (ov == 0u)
      break;
  }

  /* 4. Graceful degradation if extremely dense input still has overlaps: uniform scale up to 1.6x. */
  float worst_ratio = 1.0f;
  guint rem = count_remaining_overlaps(s, &worst_ratio);
  if (rem > 0u && worst_ratio > 1.001f) {
    float scale = MIN(worst_ratio * 1.02f, 1.6f);
    for (guint i = 0; i < s->n_vis; i++) {
      if (!s->pinned[i]) {
        s->x[i] *= scale;
        s->y[i] *= scale;
      }
    }
    for (int pass = 0; pass < 32; pass++) {
      if (run_collision_pass(s, TRUE, 1.0f) == 0u)
        break;
    }
    rem = count_remaining_overlaps(s, NULL);
  }
  s->remaining_overlaps = rem;
  if (rem > 0u)
    g_message("graphlayout: %u overlaps remained after final relaxation", rem);

  publish_to_graph(s);
}

void bt_graph_layout_free(BtGraphLayoutState *s)
{
  if (s == NULL)
    return;
  if (s->comps != NULL) {
    for (guint c = 0; c < s->n_comps; c++)
      g_free(s->comps[c].members);
    g_free(s->comps);
  }
  grid_free(&s->rep_grid);
  grid_free(&s->col_grid);
  g_free(s->vis_to_orig);
  g_free(s->orig_to_vis);
  g_free(s->x);
  g_free(s->y);
  g_free(s->x0);
  g_free(s->y0);
  g_free(s->vx);
  g_free(s->vy);
  g_free(s->rad);
  g_free(s->deg);
  g_free(s->pinned);
  g_free(s->is_halo);
  g_free(s->comp_id);
  g_free(s->edges);
  g_free(s);
}

void bt_graph_layout_run(BtGraph *g, const BtGraphLayoutOpts *o)
{
  BtGraphLayoutState *s = bt_graph_layout_begin(g, o);
  if (s == NULL)
    return;
  gdouble cap = (o != NULL) ? o->time_cap_ms : 0.0;
  bt_graph_layout_step(s, s->opts.max_iterations, cap);
  bt_graph_layout_finish(s);
  bt_graph_layout_free(s);
}

void bt_graph_layout_set_pinned(BtGraphLayoutState *s, guint node_index, gboolean pinned)
{
  if (s == NULL || node_index >= s->n_total)
    return;
  gint vi = s->orig_to_vis[node_index];
  if (vi >= 0)
    s->pinned[vi] = pinned ? 1u : 0u;
}

void bt_graph_layout_set_pos(BtGraphLayoutState *s, guint node_index, float x, float y)
{
  if (s == NULL || node_index >= s->n_total)
    return;
  gint vi = s->orig_to_vis[node_index];
  if (vi >= 0 && isfinite(x) && isfinite(y)) {
    s->x[vi] = x;
    s->y[vi] = y;
    s->vx[vi] = 0.0f;
    s->vy[vi] = 0.0f;
    if (s->g != NULL && s->g->nodes != NULL && node_index < s->g->nodes->len) {
      BtGraphNode *nd = g_ptr_array_index(s->g->nodes, node_index);
      nd->x = (gdouble)x;
      nd->y = (gdouble)y;
    }
  }
}

void bt_graph_layout_reheat(BtGraphLayoutState *s, float alpha)
{
  if (s == NULL)
    return;
  s->alpha = CLAMP(alpha, 0.05f, 1.0f);
  s->alpha_decay = powf(s->alpha_min / s->alpha,
                        1.0f / (float)MAX(s->opts.max_iterations, 1u));
  s->iter_done = 0u;
  s->quiet_streak = 0u;
  s->finished = FALSE;
}

guint bt_graph_layout_iterations_run(const BtGraphLayoutState *s)
{
  return s ? s->iter_done : 0u;
}

guint bt_graph_layout_nan_resets(const BtGraphLayoutState *s)
{
  return s ? s->nan_resets : 0u;
}

guint bt_graph_layout_remaining_overlaps(const BtGraphLayoutState *s)
{
  return s ? s->remaining_overlaps : 0u;
}
