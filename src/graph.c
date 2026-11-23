/* graph.c — Brain Graph model + deterministic spine/brainstem/cortex layout. */
#include "graph.h"
#include "linkresolve.h"
#include "noteutil.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void node_free(gpointer p)
{
  BtGraphNode *n = p;
  if (!n) return;
  g_free(n->id);
  g_free(n->label);
  g_free(n->relpath);
  g_free(n->source);
  g_free(n->dom_tag);
  g_free(n->top_folder);
  g_free(n);
}

static gdouble hash_hue(const gchar *s)
{
  if (s == NULL || s[0] == '\0')
    return 210.0;
  guint32 h = 2166136261u;
  for (const gchar *p = s; *p != '\0'; p++) {
    h ^= (guchar)*p;
    h *= 16777619u;
  }
  return (gdouble)(h % 360u);
}

static guint add_node(BtGraph *g, GHashTable *id_map, const gchar *id, const gchar *label,
                      const gchar *relpath, const gchar *source, BtNodeKind kind, gint64 created)
{
  BtGraphNode *n = g_new0(BtGraphNode, 1);
  n->id = g_strdup(id);
  n->label = g_strdup(label ? label : id);
  n->relpath = relpath ? g_strdup(relpath) : NULL;
  n->source = source ? g_strdup(source) : NULL;
  n->kind = kind;
  n->created = created;
  n->parent_idx = -1;
  n->visible = TRUE;
  n->hue = 210.0;
  n->radius = 6.0;
  n->birth_t = 1.0;

  guint idx = g->nodes->len;
  g_ptr_array_add(g->nodes, n);
  g_hash_table_insert(id_map, g_strdup(id), GUINT_TO_POINTER(idx + 1));
  return idx;
}

BtGraph *bt_graph_build(const BtVault *vault, BtIndex *index, const BtSessionStore *sessions)
{
  g_return_val_if_fail(index != NULL, NULL);
  BtGraph *g = g_new0(BtGraph, 1);
  g->nodes = g_ptr_array_new_with_free_func(node_free);
  g->edges = g_array_new(FALSE, FALSE, sizeof(BtGraphEdge));

  GHashTable *id_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *path_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *stem_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *node_rel_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  BtIndexSnapshot *snap = vault ? bt_index_snapshot_from_vault(vault) : bt_index_snapshot_new();

  /* 1. Every indexed note is ALWAYS one node (§6.C2 invariant). */
  GPtrArray *notes = bt_index_list_notes(index, NULL);
  for (guint i = 0; notes != NULL && i < notes->len; i++) {
    const BtNoteRow *r = g_ptr_array_index(notes, i);
    BtNodeKind k = (g_strcmp0(r->source, "human") == 0) ? BT_NODE_HUMAN_NOTE : BT_NODE_AI_NOTE;
    guint idx = add_node(g, id_map, r->id, r->title[0] ? r->title : r->path, r->path, r->source, k,
                         r->created);
    ((BtGraphNode *)g_ptr_array_index(g->nodes, idx))->updated = r->updated;
    g->note_node_count++;

    g_hash_table_insert(path_map, g_ascii_strdown(r->path, -1), GUINT_TO_POINTER(idx + 1));
    g_hash_table_insert(node_rel_map, g_strdup(r->id), g_strdup(r->path));
    if (vault == NULL)
      bt_index_snapshot_add_entry(snap, r->path, FALSE, (r->title && r->title[0]) ? r->title : NULL,
                                  NULL);
    gchar *base = g_path_get_basename(r->path);
    gsize bl = strlen(base);
    if (bl > 3 && g_ascii_strcasecmp(base + bl - 3, ".md") == 0)
      base[bl - 3] = '\0';
    gchar *low_stem = g_ascii_strdown(base, -1);
    if (!g_hash_table_contains(stem_map, low_stem))
      g_hash_table_insert(stem_map, low_stem, GUINT_TO_POINTER(idx + 1));
    else
      g_free(low_stem);
    g_free(base);

    /* Dominant tag from index tags table. */
    GPtrArray *tags = bt_index_list_tags(index, r->id, NULL);
    if (tags != NULL && tags->len > 0) {
      const BtTagRow *t0 = g_ptr_array_index(tags, 0);
      BtGraphNode *node = g_ptr_array_index(g->nodes, idx);
      node->dom_tag = g_strdup(t0->tag);
      node->hue = hash_hue(t0->tag);
    }
    g_clear_pointer(&tags, g_ptr_array_unref);

    /* Create import root square on brainstem if note lives under inbox/import/<tool>/ */
    if (g_str_has_prefix(r->path, "inbox/import/")) {
      gchar **parts = g_strsplit(r->path, "/", 4);
      if (parts[2] != NULL && parts[2][0] != '\0') {
        gchar *imp_id = g_strdup_printf("import:%s", parts[2]);
        if (!g_hash_table_contains(id_map, imp_id)) {
          guint imp_idx = add_node(g, id_map, imp_id, parts[2], NULL, "import",
                                   BT_NODE_IMPORT_ROOT, r->created);
          BtGraphEdge e = {.src_idx = imp_idx, .dst_idx = idx, .kind = BT_EDGE_TEMPORAL, .weight = 0.6};
          g_array_append_val(g->edges, e);
        }
        g_free(imp_id);
      }
      g_strfreev(parts);
    }
  }
  g_clear_pointer(&notes, g_ptr_array_unref);
  bt_index_snapshot_finalize(snap);

  /* 2. Edges + Tag nodes + Ghost nodes. */
  GPtrArray *edges = bt_index_list_edges(index, NULL);
  for (guint i = 0; edges != NULL && i < edges->len; i++) {
    const BtEdgeRow *er = g_ptr_array_index(edges, i);
    gpointer sp = g_hash_table_lookup(id_map, er->src);
    if (sp == NULL)
      continue;
    guint src_idx = GPOINTER_TO_UINT(sp) - 1;
    guint dst_idx = 0;
    BtEdgeKind ek = BT_EDGE_LINK;

    if (strcmp(er->kind, "tagged") == 0) {
      ek = BT_EDGE_TAGGED;
      gchar *tid = g_strdup_printf("tag:%s", er->dst);
      gpointer tp = g_hash_table_lookup(id_map, tid);
      if (tp == NULL) {
        dst_idx = add_node(g, id_map, tid, er->dst, NULL, NULL, BT_NODE_TAG, 0);
        BtGraphNode *tn = g_ptr_array_index(g->nodes, dst_idx);
        tn->hue = hash_hue(er->dst);
        tn->radius = 4.0;
      } else {
        dst_idx = GPOINTER_TO_UINT(tp) - 1;
      }
      g_free(tid);
    } else if (strcmp(er->kind, "similar") == 0) {
      ek = BT_EDGE_SIMILAR;
      gpointer dp = g_hash_table_lookup(id_map, er->dst);
      if (dp == NULL)
        continue;
      dst_idx = GPOINTER_TO_UINT(dp) - 1;
    } else {
      /* link edge: share bt_link_resolve() with editor so ghost iff UNRESOLVED (§A.5). */
      ek = BT_EDGE_LINK;
      gpointer dp = g_hash_table_lookup(id_map, er->dst);
      if (dp == NULL) {
        const gchar *src_rel = g_hash_table_lookup(node_rel_map, er->src);
        BtLinkResolution *lres = bt_link_resolve(snap, src_rel, er->dst);
        if (lres != NULL &&
            (lres->status == BT_LINK_RESOLVED || lres->status == BT_LINK_AMBIGUOUS)) {
          if (lres->kind == BT_LINK_KIND_NOTE && lres->path != NULL) {
            gchar *low_p = g_ascii_strdown(lres->path, -1);
            dp = g_hash_table_lookup(path_map, low_p);
            g_free(low_p);
          } else {
            /* Existing non-note file, directory, or same-note anchor: not a ghost note. */
            bt_link_resolution_free(lres);
            continue;
          }
        } else if (lres != NULL &&
                   (lres->status == BT_LINK_EXTERNAL || lres->status == BT_LINK_PLACEHOLDER)) {
          bt_link_resolution_free(lres);
          continue;
        }

        if (dp != NULL) {
          dst_idx = GPOINTER_TO_UINT(dp) - 1;
        } else {
          gboolean broken = (lres != NULL && lres->status == BT_LINK_BLOCKED);
          gchar *gid = g_strdup_printf("ghost:%s", er->dst);
          gpointer gp = g_hash_table_lookup(id_map, gid);
          if (gp == NULL) {
            gchar *new_rel = broken ? NULL : bt_wikilink_new_note_path(er->dst);
            if (!broken && vault != NULL && new_rel != NULL) {
              gchar *chk = vault_resolve(vault, new_rel, NULL);
              broken = (chk == NULL);
              g_free(chk);
            }
            dst_idx = add_node(g, id_map, gid, er->dst, broken ? NULL : new_rel, NULL,
                               BT_NODE_GHOST, 0);
            BtGraphNode *gn = g_ptr_array_index(g->nodes, dst_idx);
            gn->broken = broken;
            gn->radius = 5.5;
            g->ghost_node_count++;
            g_free(new_rel);
          } else {
            dst_idx = GPOINTER_TO_UINT(gp) - 1;
          }
          g_free(gid);
        }
        bt_link_resolution_free(lres);
      } else {
        dst_idx = GPOINTER_TO_UINT(dp) - 1;
      }
    }

    if (src_idx != dst_idx) {
      BtGraphEdge e = {.src_idx = src_idx, .dst_idx = dst_idx, .kind = ek, .weight = er->weight};
      g_array_append_val(g->edges, e);
      ((BtGraphNode *)g_ptr_array_index(g->nodes, src_idx))->degree++;
      ((BtGraphNode *)g_ptr_array_index(g->nodes, dst_idx))->degree++;
    }
  }
  g_clear_pointer(&edges, g_ptr_array_unref);

  /* 3. Session square nodes on the brainstem (§6.C2). */
  if (sessions != NULL) {
    const GPtrArray *slist = bt_sessions_list(sessions);
    for (guint i = 0; slist != NULL && i < slist->len; i++) {
      const BtSession *s = g_ptr_array_index(slist, i);
      gchar *sid = g_strdup_printf("session:%s", s->id);
      guint sidx = add_node(g, id_map, sid, s->tool, NULL, "ai", BT_NODE_SESSION, s->started);
      if (s->note_id != NULL) {
        gchar *low = g_ascii_strdown(s->note_id, -1);
        gpointer np = g_hash_table_lookup(path_map, low);
        g_free(low);
        if (np != NULL) {
          BtGraphEdge e = {
            .src_idx = sidx, .dst_idx = GPOINTER_TO_UINT(np) - 1,
            .kind = BT_EDGE_TEMPORAL, .weight = 0.5,
          };
          g_array_append_val(g->edges, e);
        }
      }
      g_free(sid);
    }
  }

  /* 3b. First-class folder nodes from directory hierarchy (Phase 1). */
  {
    GHashTable *folder_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint base_note_limit = g->nodes->len;
    for (guint i = 0; i < base_note_limit; i++) {
      BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      if ((nd->kind != BT_NODE_HUMAN_NOTE && nd->kind != BT_NODE_AI_NOTE) ||
          nd->relpath == NULL)
        continue;

      /* Normalize separators and split directory components. */
      g_autofree gchar *norm = g_strdup(nd->relpath);
      for (gchar *p = norm; *p != '\0'; p++) {
        if (*p == '\\')
          *p = '/';
      }
      gchar *slash = strrchr(norm, '/');
      if (slash == NULL)
        continue; /* vault root note: parent_idx = -1, top_folder = NULL */
      *slash = '\0';

      gchar **parts = g_strsplit(norm, "/", -1);
      gint cur_parent = -1;
      GString *acc = g_string_new(NULL);
      const gchar *top_name = (parts[0] != NULL && parts[0][0] != '\0') ? parts[0] : NULL;

      for (guint p = 0; parts[p] != NULL && parts[p][0] != '\0'; p++) {
        if (acc->len > 0)
          g_string_append_c(acc, '/');
        g_string_append(acc, parts[p]);

        gpointer fp = g_hash_table_lookup(folder_map, acc->str);
        guint fidx = 0;
        if (fp == NULL) {
          g_autofree gchar *fid = g_strdup_printf("folder:%s", acc->str);
          fidx = add_node(g, id_map, fid, parts[p], acc->str, NULL, BT_NODE_FOLDER, 0);
          BtGraphNode *fn = g_ptr_array_index(g->nodes, fidx);
          fn->parent_idx = cur_parent;
          fn->top_folder = g_strdup(top_name);
          fn->hue = hash_hue(top_name ? top_name : acc->str);
          g->folder_node_count++;
          g_hash_table_insert(folder_map, g_strdup(acc->str), GUINT_TO_POINTER(fidx + 1));

          if (cur_parent >= 0) {
            BtGraphEdge fe = {
              .src_idx = (guint)cur_parent,
              .dst_idx = fidx,
              .kind = BT_EDGE_CONTAINS,
              .weight = 0.35,
            };
            g_array_append_val(g->edges, fe);
          }
        } else {
          fidx = GPOINTER_TO_UINT(fp) - 1;
        }
        ((BtGraphNode *)g_ptr_array_index(g->nodes, fidx))->child_count++;
        cur_parent = (gint)fidx;
      }

      /* Re-fetch nd in case g->nodes reallocated during add_node. */
      nd = g_ptr_array_index(g->nodes, i);
      nd->parent_idx = cur_parent;
      g_free(nd->top_folder);
      nd->top_folder = top_name ? g_strdup(top_name) : NULL;
      if (cur_parent >= 0) {
        BtGraphEdge te = {
          .src_idx = (guint)cur_parent,
          .dst_idx = i,
          .kind = BT_EDGE_CONTAINS,
          .weight = 0.25,
        };
        g_array_append_val(g->edges, te);
      }

      g_string_free(acc, TRUE);
      g_strfreev(parts);
    }
    g_hash_table_destroy(folder_map);
  }

  /* Update note & folder radii proportional to degree / child count. */
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *n = g_ptr_array_index(g->nodes, i);
    if (n->kind == BT_NODE_HUMAN_NOTE || n->kind == BT_NODE_AI_NOTE)
      n->radius = CLAMP(4.0 + sqrt((gdouble)n->degree) * 1.9, 4.0, 14.0);
    else if (n->kind == BT_NODE_FOLDER)
      n->radius = CLAMP(7.5 + sqrt((gdouble)n->child_count) * 1.1, 8.0, 14.0);
    else if (n->kind == BT_NODE_IMPORT_ROOT)
      n->radius = 10.0;
    else if (n->kind == BT_NODE_SESSION)
      n->radius = 7.0;
  }

  bt_graph_apply_default_collapse(g);

  bt_index_snapshot_free(snap);
  g_hash_table_destroy(node_rel_map);
  g_hash_table_destroy(id_map);
  g_hash_table_destroy(path_map);
  g_hash_table_destroy(stem_map);
  return g;
}

void bt_graph_free(BtGraph *g)
{
  if (g == NULL)
    return;
  g_ptr_array_unref(g->nodes);
  g_array_unref(g->edges);
  g_free(g);
}

void bt_graph_apply_default_collapse(BtGraph *g)
{
  if (g == NULL || g->nodes == NULL)
    return;
  gboolean collapse_sub = (g->nodes->len > 1500u);
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->kind == BT_NODE_FOLDER)
      nd->collapsed = collapse_sub && (nd->parent_idx >= 0);
  }
  bt_graph_recompute_visibility(g, TRUE, TRUE);
}

gboolean bt_graph_is_descendant_of(const BtGraph *g, guint node_idx, guint folder_idx)
{
  if (g == NULL || g->nodes == NULL || node_idx >= g->nodes->len || folder_idx >= g->nodes->len)
    return FALSE;
  const BtGraphNode *cur = g_ptr_array_index(g->nodes, node_idx);
  for (guint guard = 0; guard < 64u && cur->parent_idx >= 0; guard++) {
    if ((guint)cur->parent_idx == folder_idx)
      return TRUE;
    if ((guint)cur->parent_idx >= g->nodes->len)
      break;
    cur = g_ptr_array_index(g->nodes, (guint)cur->parent_idx);
  }
  return FALSE;
}

void bt_graph_set_folder_collapsed(BtGraph *g, guint folder_idx, gboolean collapsed)
{
  if (g == NULL || g->nodes == NULL || folder_idx >= g->nodes->len)
    return;
  BtGraphNode *fn = g_ptr_array_index(g->nodes, folder_idx);
  if (fn->kind != BT_NODE_FOLDER)
    return;
  fn->collapsed = collapsed;
}

void bt_graph_set_folder_hidden(BtGraph *g, guint folder_idx, gboolean hidden)
{
  if (g == NULL || g->nodes == NULL || folder_idx >= g->nodes->len)
    return;
  BtGraphNode *fn = g_ptr_array_index(g->nodes, folder_idx);
  if (fn->kind != BT_NODE_FOLDER)
    return;
  fn->hidden_by_user = hidden;
}

void bt_graph_recompute_visibility(BtGraph *g, gboolean show_tags, gboolean show_ghosts)
{
  if (g == NULL || g->nodes == NULL)
    return;
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    gboolean vis = TRUE;
    if (nd->kind == BT_NODE_TAG && !show_tags)
      vis = FALSE;
    else if (nd->kind == BT_NODE_GHOST && !show_ghosts)
      vis = FALSE;
    else if (nd->hidden_by_user)
      vis = FALSE;

    if (vis && nd->parent_idx >= 0) {
      gint p = nd->parent_idx;
      for (guint guard = 0; guard < 64u && p >= 0 && (guint)p < g->nodes->len; guard++) {
        const BtGraphNode *pnd = g_ptr_array_index(g->nodes, (guint)p);
        if (pnd->collapsed || pnd->hidden_by_user) {
          vis = FALSE;
          break;
        }
        p = pnd->parent_idx;
      }
    }
    nd->visible = vis;
  }
}

void bt_graph_layout(BtGraph *g, guint relax_iters)
{
  g_return_if_fail(g != NULL);
  guint n = g->nodes->len;
  if (n == 0)
    return;

  /* 1. Forced Brainstem (bottom band y = +260) and Forced Spine (center x = 0). */
  guint stem_count = 0;
  for (guint i = 0; i < n; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT)
      stem_count++;
  }
  guint stem_i = 0;
  guint spine_i = 0;

  for (guint i = 0; i < n; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT) {
      gdouble spread = (gdouble)MAX(stem_count, 1u) * 32.0;
      nd->x = -spread * 0.5 + ((gdouble)stem_i + 0.5) * 32.0;
      nd->y = 260.0;
      stem_i++;
    } else if ((nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE) &&
               nd->degree == 0) {
      /* Isolated note attaches directly to the vertical spine (deterministic). */
      nd->x = (spine_i % 2 == 0) ? -14.0 : 14.0;
      nd->y = 180.0 - (gdouble)(spine_i % 40) * 10.0;
      spine_i++;
    } else {
      /* Cortex radial placement by dominant tag hue (deterministic). */
      gdouble angle = (nd->hue / 360.0) * 2.0 * M_PI + ((gdouble)(i % 17) * 0.09);
      gdouble rad = 85.0 + (gdouble)((i * 29) % 160);
      if (nd->kind == BT_NODE_TAG)
        rad = 65.0;
      else if (nd->kind == BT_NODE_GHOST)
        rad = 220.0 + (gdouble)((i * 13) % 50);
      nd->x = cos(angle) * rad;
      nd->y = -40.0 + sin(angle) * rad * 0.85;
    }
  }

  /* 2. Cortex force relaxation (brainstem & spine-attached isolated nodes stay forced). */
  guint iters = relax_iters > 0 ? relax_iters : 25u;
  gdouble *dx = g_new0(gdouble, n);
  gdouble *dy = g_new0(gdouble, n);

  for (guint it = 0; it < iters; it++) {
    memset(dx, 0, sizeof(gdouble) * n);
    memset(dy, 0, sizeof(gdouble) * n);

    /* Edge attraction. */
    for (guint e = 0; e < g->edges->len; e++) {
      const BtGraphEdge *ed = &g_array_index(g->edges, BtGraphEdge, e);
      BtGraphNode *a = g_ptr_array_index(g->nodes, ed->src_idx);
      BtGraphNode *b = g_ptr_array_index(g->nodes, ed->dst_idx);
      gdouble vx = b->x - a->x;
      gdouble vy = b->y - a->y;
      gdouble dist = sqrt(vx * vx + vy * vy) + 1.0;
      gdouble f = (dist - 42.0) * 0.04 * ed->weight;
      gdouble fx = (vx / dist) * f;
      gdouble fy = (vy / dist) * f;
      dx[ed->src_idx] += fx;
      dy[ed->src_idx] += fy;
      dx[ed->dst_idx] -= fx;
      dy[ed->dst_idx] -= fy;
    }

    /* Local repulsion (bounded window for O(n) scaling at 1k–10k nodes). */
    guint stride = n > 200 ? 12u : n;
    for (guint i = 0; i < n; i++) {
      BtGraphNode *a = g_ptr_array_index(g->nodes, i);
      for (guint k = 1; k < stride && i + k < n; k++) {
        guint j = i + k;
        BtGraphNode *b = g_ptr_array_index(g->nodes, j);
        gdouble vx = a->x - b->x;
        gdouble vy = a->y - b->y;
        gdouble d2 = vx * vx + vy * vy + 16.0;
        if (d2 < 3600.0) {
          gdouble f = 140.0 / d2;
          dx[i] += vx * f;
          dy[i] += vy * f;
          dx[j] -= vx * f;
          dy[j] -= vy * f;
        }
      }
    }

    for (guint i = 0; i < n; i++) {
      BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      /* Brainstem & isolated spine nodes remain strictly forced! */
      if (nd->kind == BT_NODE_SESSION || nd->kind == BT_NODE_IMPORT_ROOT ||
          ((nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE) && nd->degree == 0))
        continue;
      nd->x += CLAMP(dx[i], -12.0, 12.0);
      nd->y += CLAMP(dy[i], -12.0, 12.0);
    }
  }

  g_free(dx);
  g_free(dy);
}

void bt_graph_apply_filter(BtGraph *g, const BtGraphFilter *f)
{
  g_return_if_fail(g != NULL);
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    gboolean vis = TRUE;
    if (f != NULL) {
      if (f->source_filter && f->source_filter[0] &&
          (nd->kind == BT_NODE_HUMAN_NOTE || nd->kind == BT_NODE_AI_NOTE)) {
        if (g_strcmp0(nd->source, f->source_filter) != 0)
          vis = FALSE;
      }
      if (vis && f->tag_filter && f->tag_filter[0]) {
        if (nd->kind == BT_NODE_TAG)
          vis = g_ascii_strcasecmp(nd->label, f->tag_filter) == 0;
        else if (nd->dom_tag != NULL)
          vis = g_ascii_strcasecmp(nd->dom_tag, f->tag_filter) == 0;
        else
          vis = FALSE;
      }
      if (vis && f->min_created > 0 && nd->created > 0 && nd->created < f->min_created)
        vis = FALSE;
      if (vis && f->max_created > 0 && nd->created > 0 && nd->created > f->max_created)
        vis = FALSE;
    }
    nd->visible = vis;
  }
}

gint bt_graph_pick(const BtGraph *g, gdouble wx, gdouble wy, gdouble hit_radius)
{
  if (g == NULL)
    return -1;
  gint best = -1;
  gdouble best_d2 = hit_radius * hit_radius;
  for (guint i = 0; i < g->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (!nd->visible)
      continue;
    gdouble dx = nd->x - wx;
    gdouble dy = nd->y - wy;
    gdouble r = MAX(nd->radius + 3.0, hit_radius);
    gdouble d2 = dx * dx + dy * dy;
    if (d2 <= r * r && d2 <= best_d2) {
      best_d2 = d2;
      best = (gint)i;
    }
  }
  return best;
}
