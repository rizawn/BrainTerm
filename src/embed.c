/* embed.c — Local offline hashed n-gram vectorizer + cosine + RRF hybrid search (ADR-005). */
#include "embed.h"
#include "frontmatter.h"
#include "mdparse.h"

#include <math.h>
#include <string.h>

static guint32 fnv32(const gchar *s, gsize len)
{
  guint32 h = 2166136261u;
  for (gsize i = 0; i < len; i++) {
    h ^= (guchar)s[i];
    h *= 16777619u;
  }
  return h;
}

void bt_embed_text(const gchar *text, gfloat out_vec[BT_EMBED_DIM])
{
  memset(out_vec, 0, sizeof(gfloat) * BT_EMBED_DIM);
  if (text == NULL || text[0] == '\0')
    return;

  gchar *low = g_ascii_strdown(text, -1);
  gchar **toks = g_strsplit_set(low, " \t\r\n.,;:!?()[]{}<>\"'`/#-_", -1);

  for (guint i = 0; toks[i] != NULL; i++) {
    const gchar *t = toks[i];
    gsize tl = strlen(t);
    if (tl < 2)
      continue;
    /* Word hash */
    guint32 hw = fnv32(t, tl);
    out_vec[hw % BT_EMBED_DIM] += (hw & 1u) ? 2.0f : -2.0f;
    /* Character trigrams for morphological / fuzzy semantic overlap */
    if (tl >= 3) {
      for (gsize j = 0; j + 3 <= tl; j++) {
        guint32 hg = fnv32(t + j, 3);
        out_vec[hg % BT_EMBED_DIM] += (hg & 1u) ? 1.0f : -1.0f;
      }
    }
  }
  g_strfreev(toks);
  g_free(low);

  gdouble norm = 0.0;
  for (guint i = 0; i < BT_EMBED_DIM; i++)
    norm += (gdouble)out_vec[i] * (gdouble)out_vec[i];
  norm = sqrt(norm);
  if (norm > 1e-9) {
    for (guint i = 0; i < BT_EMBED_DIM; i++)
      out_vec[i] = (gfloat)((gdouble)out_vec[i] / norm);
  }
}

gdouble bt_embed_cosine(const gfloat a[BT_EMBED_DIM], const gfloat b[BT_EMBED_DIM])
{
  gdouble dot = 0.0;
  for (guint i = 0; i < BT_EMBED_DIM; i++)
    dot += (gdouble)a[i] * (gdouble)b[i];
  return dot;
}

typedef struct {
  gchar *id;
  gchar *path;
  gchar *title;
  gfloat vec[BT_EMBED_DIM];
} NoteVec;

static void note_vec_free(gpointer p)
{
  NoteVec *nv = p;
  if (!nv) return;
  g_free(nv->id);
  g_free(nv->path);
  g_free(nv->title);
  g_free(nv);
}

gboolean bt_embed_sync_vault(const BtVault *vault, BtIndex *index, gboolean enabled,
                             guint *similar_edges_out, GError **error)
{
  g_return_val_if_fail(vault != NULL && index != NULL, FALSE);
  if (similar_edges_out)
    *similar_edges_out = 0;
  if (!bt_index_clear_embeddings(index, error))
    return FALSE;
  if (!enabled)
    return TRUE;

  GPtrArray *notes = bt_index_list_notes(index, error);
  if (notes == NULL)
    return FALSE;

  GPtrArray *vecs = g_ptr_array_new_with_free_func(note_vec_free);
  for (guint i = 0; i < notes->len; i++) {
    const BtNoteRow *r = g_ptr_array_index(notes, i);
    gchar *raw = NULL;
    gsize len = 0;
    if (!vault_read(vault, r->path, &raw, &len, NULL))
      continue;
    BtFrontmatter *fm = bt_frontmatter_parse(raw, len);
    BtMdExtract *ex = bt_mdparse_extract(raw + fm->body_offset, len - fm->body_offset);

    NoteVec *nv = g_new0(NoteVec, 1);
    nv->id = g_strdup(r->id);
    nv->path = g_strdup(r->path);
    nv->title = g_strdup(r->title);
    gchar *combined = g_strdup_printf("%s %s", r->title, ex->plain_body);
    bt_embed_text(combined, nv->vec);
    g_free(combined);

    bt_index_put_embedding(index, nv->id, 0, nv->vec, BT_EMBED_DIM, NULL);
    g_ptr_array_add(vecs, nv);

    bt_mdparse_free(ex);
    bt_frontmatter_free(fm);
    g_free(raw);
  }
  g_ptr_array_unref(notes);

  guint sim_edges = 0;
  for (guint i = 0; i < vecs->len; i++) {
    const NoteVec *a = g_ptr_array_index(vecs, i);
    for (guint j = i + 1; j < vecs->len; j++) {
      const NoteVec *b = g_ptr_array_index(vecs, j);
      gdouble cos = bt_embed_cosine(a->vec, b->vec);
      if (cos > BT_SIMILAR_THRESHOLD) {
        bt_index_put_edge(index, a->id, b->id, "similar", cos, NULL);
        sim_edges++;
      }
    }
  }
  if (similar_edges_out)
    *similar_edges_out = sim_edges;
  g_ptr_array_unref(vecs);
  return TRUE;
}

static gint cmp_hit_score_desc(gconstpointer a, gconstpointer b)
{
  const BtSearchHit *ha = *(const BtSearchHit *const *)a;
  const BtSearchHit *hb = *(const BtSearchHit *const *)b;
  if (ha->rank > hb->rank) return -1;
  if (ha->rank < hb->rank) return 1;
  return strcmp(ha->path, hb->path);
}

GPtrArray *bt_embed_hybrid_search(const BtVault *vault, BtIndex *index, const gchar *query,
                                  guint limit, gboolean enabled, GError **error)
{
  GPtrArray *fts = bt_index_search_fts(index, query, limit > 0 ? limit : 25, error);
  if (!enabled || fts == NULL || query == NULL || query[0] == '\0')
    return fts;

  /* RRF merge: FTS ranks + offline vector cosine ranks (k = 60). */
  GHashTable *by_path = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < fts->len; i++) {
    BtSearchHit *h = g_ptr_array_index(fts, i);
    h->rank = 1.0 / (60.0 + (gdouble)(i + 1));
    g_hash_table_insert(by_path, h->path, h);
  }

  gfloat qvec[BT_EMBED_DIM];
  bt_embed_text(query, qvec);

  GPtrArray *notes = bt_index_list_notes(index, NULL);
  for (guint i = 0; notes != NULL && i < notes->len; i++) {
    const BtNoteRow *r = g_ptr_array_index(notes, i);
    gchar *raw = NULL;
    gsize len = 0;
    if (!vault_read(vault, r->path, &raw, &len, NULL))
      continue;
    gfloat nvec[BT_EMBED_DIM];
    bt_embed_text(raw, nvec);
    gdouble cos = bt_embed_cosine(qvec, nvec);
    g_free(raw);
    if (cos < 0.25)
      continue;

    BtSearchHit *existing = g_hash_table_lookup(by_path, r->path);
    if (existing != NULL) {
      existing->rank += cos * 0.05;
    } else {
      BtSearchHit *h = g_new0(BtSearchHit, 1);
      h->note_id = g_strdup(r->id);
      h->path = g_strdup(r->path);
      h->title = g_strdup(r->title);
      h->snippet = g_strdup_printf("[semantic match %.2f]", cos);
      h->rank = cos * 0.05;
      g_ptr_array_add(fts, h);
      g_hash_table_insert(by_path, h->path, h);
    }
  }
  g_clear_pointer(&notes, g_ptr_array_unref);
  g_hash_table_destroy(by_path);

  g_ptr_array_sort(fts, cmp_hit_score_desc);
  guint cap = limit > 0 ? limit : 25;
  while (fts->len > cap)
    g_ptr_array_remove_index(fts, fts->len - 1);
  return fts;
}
