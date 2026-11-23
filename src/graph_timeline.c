/* graph_timeline.c — Pure-C Timeline date parsing, wavy lane beeswarm packing,
 * Kept grid packing, and ledger event extraction. */
#include "graph_timeline.h"
#include "graph_palette.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

gint64 bt_timeline_parse_date(const gchar *date_str)
{
  if (date_str == NULL || date_str[0] == '\0')
    return 0;
  /* Fast YYYY-MM-DD prefix parse (handles "2026-11-23" and "2026-11-23T12:00:00Z"). */
  int y = 0, m = 0, d = 0;
  if (sscanf(date_str, "%4d-%2d-%2d", &y, &m, &d) == 3 &&
      y >= 1970 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
    GDateTime *dt = g_date_time_new_utc(y, m, d, 12, 0, 0.0);
    if (dt != NULL) {
      gint64 ts = g_date_time_to_unix(dt);
      g_date_time_unref(dt);
      return ts;
    }
  }
  GDateTime *iso = g_date_time_new_from_iso8601(date_str, NULL);
  if (iso != NULL) {
    gint64 ts = g_date_time_to_unix(iso);
    g_date_time_unref(iso);
    return ts;
  }
  return 0;
}

static gint cmp_str_ci(gconstpointer a, gconstpointer b)
{
  return g_ascii_strcasecmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static guint32 hash_u32_tl(guint32 x)
{
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

static gint cmp_ledger_ts(const void *a, const void *b)
{
  const BtLedgerEvent *ea = a;
  const BtLedgerEvent *eb = b;
  if (ea->ts < eb->ts) return -1;
  if (ea->ts > eb->ts) return 1;
  return 0;
}

BtTimelineLayout *bt_timeline_build(const BtGraph *g, BtTimelineLaneBy lane_by,
                                    float x0, float x1, float y0, float y1,
                                    gint64 view_min_ts, gint64 view_max_ts)
{
  BtTimelineLayout *tl = g_new0(BtTimelineLayout, 1);
  if (g == NULL || g->nodes == NULL || g->nodes->len == 0u)
    return tl;

  guint n = g->nodes->len;
  tl->dots = g_new0(BtTimelineDot, n);
  tl->dot_count = n;

  /* 1. Determine time bounds across all dated nodes (prefer created, fallback updated). */
  gint64 min_t = G_MAXINT64, max_t = 0;
  gint64 now_ts = g_get_real_time() / G_USEC_PER_SEC;

  GPtrArray *lane_list = g_ptr_array_new();
  GHashTable *lane_map = g_hash_table_new(g_str_hash, g_str_equal);
  GArray *ev_arr = g_array_new(FALSE, FALSE, sizeof(BtLedgerEvent));

  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    gint64 ts = (nd->created > 0) ? nd->created : nd->updated;

    /* Collect ledger events for Session / Import / AI Edit nodes. */
    if (ts > 0 && nd->kind == BT_NODE_SESSION) {
      BtLedgerEvent ev = {.ts = ts, .kind = BT_LEDGER_SESSION, .color_rgb = 0x5EE0F2u};
      g_strlcpy(ev.label, "SESSION", sizeof(ev.label));
      g_array_append_val(ev_arr, ev);
    } else if (ts > 0 && nd->kind == BT_NODE_IMPORT_ROOT) {
      BtLedgerEvent ev = {.ts = ts, .kind = BT_LEDGER_IMPORT, .color_rgb = 0x7BE495u};
      g_strlcpy(ev.label, "IMPORT", sizeof(ev.label));
      g_array_append_val(ev_arr, ev);
    } else if (ts > 0 && nd->kind == BT_NODE_AI_NOTE && g_strcmp0(nd->source, "ai") == 0) {
      BtLedgerEvent ev = {.ts = ts, .kind = BT_LEDGER_AI_EDIT, .color_rgb = 0xB58CE6u};
      g_strlcpy(ev.label, "AI EDIT", sizeof(ev.label));
      g_array_append_val(ev_arr, ev);
    }

    if (!nd->visible || nd->kind == BT_NODE_FOLDER || nd->kind == BT_NODE_TAG ||
        nd->kind == BT_NODE_GHOST)
      continue;
    if (ts <= 0)
      ts = now_ts;
    if (ts < min_t) min_t = ts;
    if (ts > max_t) max_t = ts;

    const gchar *lane_key = NULL;
    if (lane_by == BT_TL_LANE_TYPE) {
      if (nd->kind == BT_NODE_HUMAN_NOTE) lane_key = "Notes";
      else if (nd->kind == BT_NODE_AI_NOTE) lane_key = "AI Edits";
      else if (nd->kind == BT_NODE_SESSION) lane_key = "Sessions";
      else lane_key = "Imports";
    } else {
      lane_key = (nd->top_folder && nd->top_folder[0]) ? nd->top_folder : "(root)";
    }
    if (!g_hash_table_contains(lane_map, lane_key)) {
      g_hash_table_insert(lane_map, (gpointer)lane_key, GUINT_TO_POINTER(1));
      g_ptr_array_add(lane_list, (gpointer)lane_key);
    }
  }

  if (min_t == G_MAXINT64) {
    min_t = now_ts - 86400 * 30;
    max_t = now_ts;
  }
  if (max_t <= min_t)
    max_t = min_t + 86400;

  tl->min_ts = min_t;
  tl->max_ts = max_t;

  gint64 vt0 = (view_min_ts > 0 && view_max_ts > view_min_ts) ? view_min_ts : min_t;
  gint64 vt1 = (view_min_ts > 0 && view_max_ts > view_min_ts) ? view_max_ts : max_t;
  gdouble t_span = MAX((gdouble)(vt1 - vt0), 60.0);

  g_ptr_array_sort(lane_list, cmp_str_ci);
  tl->lane_count = MAX(lane_list->len, 1u);
  tl->lane_names = g_new0(gchar *, tl->lane_count);
  tl->lane_colors = g_new0(guint32, tl->lane_count);

  GHashTable *lane_idx_map = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint li = 0; li < lane_list->len; li++) {
    const gchar *nm = g_ptr_array_index(lane_list, li);
    tl->lane_names[li] = g_strdup(nm);
    tl->lane_colors[li] = BT_GALAXY_ROOT_COLOR;
    g_hash_table_insert(lane_idx_map, (gpointer)nm, GUINT_TO_POINTER(li));
  }
  if (lane_list->len == 0u) {
    tl->lane_names[0] = g_strdup("(root)");
    tl->lane_colors[0] = BT_GALAXY_ROOT_COLOR;
  }

  /* Add synthetic CHECKPOINT markers at timeline quartiles if no events exist yet. */
  if (ev_arr->len == 0u) {
    BtLedgerEvent ck = {.ts = min_t + (max_t - min_t) / 2,
                        .kind = BT_LEDGER_CHECKPOINT,
                        .color_rgb = 0xFFC857u};
    g_strlcpy(ck.label, "CHECKPOINT", sizeof(ck.label));
    g_array_append_val(ev_arr, ck);
  }
  /* Thin ledger events to at most 16 readable markers. */
  qsort(ev_arr->data, ev_arr->len, sizeof(BtLedgerEvent), cmp_ledger_ts);
  guint max_ev = 16u;
  guint step_ev = (ev_arr->len > max_ev) ? ((ev_arr->len + max_ev - 1u) / max_ev) : 1u;
  guint out_ev = 0u;
  tl->events = g_new0(BtLedgerEvent, MIN(ev_arr->len, max_ev));
  for (guint i = 0; i < ev_arr->len && out_ev < max_ev; i += step_ev)
    tl->events[out_ev++] = g_array_index(ev_arr, BtLedgerEvent, i);
  tl->event_count = out_ev;
  g_array_unref(ev_arr);

  /* 2. Initial Organic placement along undulating wavy horizontal lane bands. */
  float w = MAX(x1 - x0, 40.0f);
  float h = MAX(y1 - y0, 40.0f);
  float lane_h = h / (float)tl->lane_count;
  guint valid_count = 0u;

  for (guint i = 0; i < n; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (!nd->visible || nd->kind == BT_NODE_FOLDER || nd->kind == BT_NODE_TAG ||
        nd->kind == BT_NODE_GHOST)
      continue;
    gint64 ts = (nd->created > 0) ? nd->created : (nd->updated > 0 ? nd->updated : now_ts);
    const gchar *lane_key = (lane_by == BT_TL_LANE_TYPE)
        ? ((nd->kind == BT_NODE_HUMAN_NOTE) ? "Notes"
           : (nd->kind == BT_NODE_AI_NOTE ? "AI Edits"
              : (nd->kind == BT_NODE_SESSION ? "Sessions" : "Imports")))
        : ((nd->top_folder && nd->top_folder[0]) ? nd->top_folder : "(root)");
    guint li = GPOINTER_TO_UINT(g_hash_table_lookup(lane_idx_map, lane_key));
    if (nd->color_rgb != 0u && tl->lane_colors[li] == BT_GALAXY_ROOT_COLOR)
      tl->lane_colors[li] = nd->color_rgb;

    gdouble u = ((gdouble)(ts - vt0)) / t_span;
    float sx = x0 + (float)(u * (gdouble)w);
    guint32 rh = hash_u32_tl(i * 2654435761u + 19u);
    float jitter = ((float)(rh & 0xFFFFu) / 65535.0f - 0.5f) * MIN(lane_h * 0.48f, 18.0f);
    float phase = (float)li * 1.15f;
    float wave = sinf((float)u * 6.28318f * 1.4f + phase) * MIN(lane_h * 0.24f, 14.0f);
    float sy = y0 + ((float)li + 0.5f) * lane_h + wave + jitter;

    BtTimelineDot *dot = &tl->dots[i];
    dot->node_idx = i;
    dot->ts = ts;
    dot->lane_idx = li;
    dot->org_x = sx;
    dot->org_y = CLAMP(sy, y0 + 6.0f, y1 - 6.0f);
    dot->radius = CLAMP(3.2f + sqrtf((float)nd->degree) * 0.55f, 3.2f, 6.5f);
    dot->valid = TRUE;
    valid_count++;
  }

  /* 3. Beeswarm non-overlap relaxation for Organic mode (bounded window O(N)). */
  for (int pass = 0; pass < 6; pass++) {
    for (guint i = 0; i < n; i++) {
      if (!tl->dots[i].valid)
        continue;
      for (guint j = i + 1; j < n; j++) {
        if (!tl->dots[j].valid)
          continue;
        float dx = tl->dots[j].org_x - tl->dots[i].org_x;
        float min_d = tl->dots[i].radius + tl->dots[j].radius + 1.5f;
        if (fabsf(dx) >= min_d)
          continue;
        float dy = tl->dots[j].org_y - tl->dots[i].org_y;
        float d2 = dx * dx + dy * dy;
        if (d2 < min_d * min_d) {
          float d = sqrtf(MAX(d2, 0.01f));
          float push = 0.52f * (min_d - d);
          /* Push predominantly vertically so X stays true to timestamp. */
          float ny = (d > 0.05f) ? (dy / d) : ((i & 1u) ? 1.0f : -1.0f);
          float nx = (d > 0.05f) ? (dx / d) * 0.25f : 0.0f;
          tl->dots[i].org_x -= nx * push;
          tl->dots[i].org_y = CLAMP(tl->dots[i].org_y - ny * push, y0 + 6.0f, y1 - 6.0f);
          tl->dots[j].org_x += nx * push;
          tl->dots[j].org_y = CLAMP(tl->dots[j].org_y + ny * push, y0 + 6.0f, y1 - 6.0f);
        }
      }
    }
  }

  /* 4. Kept mode: tidy non-overlapping grid inside framed box [x0+24 .. x1-24, y0+18 .. y1-18]. */
  if (valid_count > 0u) {
    float bx0 = x0 + 24.0f;
    float bx1 = x1 - 24.0f;
    float by0 = y0 + 18.0f;
    float by1 = y1 - 18.0f;
    float bw = MAX(bx1 - bx0, 40.0f);
    float bh = MAX(by1 - by0, 40.0f);
    float aspect = bw / bh;
    guint cols = (guint)MAX(ceilf(sqrtf((float)valid_count * aspect)), 1.0f);
    guint rows = (valid_count + cols - 1u) / cols;
    float cell_w = bw / (float)MAX(cols, 1u);
    float cell_h = bh / (float)MAX(rows, 1u);

    guint k = 0u;
    for (guint li = 0; li < tl->lane_count; li++) {
      for (guint i = 0; i < n; i++) {
        if (!tl->dots[i].valid || tl->dots[i].lane_idx != li)
          continue;
        guint c = k % cols;
        guint r = k / cols;
        tl->dots[i].kept_x = bx0 + ((float)c + 0.5f) * cell_w;
        tl->dots[i].kept_y = by0 + ((float)r + 0.5f) * cell_h;
        k++;
      }
    }
  }

  g_hash_table_destroy(lane_idx_map);
  g_hash_table_destroy(lane_map);
  g_ptr_array_unref(lane_list);
  return tl;
}

void bt_timeline_free(BtTimelineLayout *tl)
{
  if (tl == NULL)
    return;
  g_free(tl->dots);
  if (tl->lane_names != NULL) {
    for (guint i = 0; i < tl->lane_count; i++)
      g_free(tl->lane_names[i]);
    g_free(tl->lane_names);
  }
  g_free(tl->lane_colors);
  g_free(tl->events);
  g_free(tl);
}
