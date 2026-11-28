/* embed.h — Optional local offline embeddings + cosine similarity edges +
 * hybrid FTS+vector search (ADR-005, M8). Default OFF; FTS never degrades. */
#ifndef BT_EMBED_H
#define BT_EMBED_H

#include <glib.h>

#include "index.h"
#include "vault.h"

G_BEGIN_DECLS

#define BT_EMBED_DIM 128u
#define BT_SIMILAR_THRESHOLD 0.75

/* Computes a normalized local offline feature vector (length BT_EMBED_DIM)
 * for text (zero network calls, P1). */
void bt_embed_text(const gchar *text, gfloat out_vec[BT_EMBED_DIM]);

/* Cosine similarity in [-1, 1] between two normalized vectors. */
gdouble bt_embed_cosine(const gfloat a[BT_EMBED_DIM], const gfloat b[BT_EMBED_DIM]);

/* Indexes embeddings for all vault notes and inserts kind='similar' edges
 * where cosine > BT_SIMILAR_THRESHOLD. When enabled == FALSE, clears vectors
 * and returns TRUE cleanly (disableable per ADR-005). */
gboolean bt_embed_sync_vault(const BtVault *vault, BtIndex *index, gboolean enabled,
                             guint *similar_edges_out, GError **error);

/* Hybrid search: merges FTS5 + vector cosine via Reciprocal Rank Fusion (RRF)
 * when enabled == TRUE; pure FTS5 when enabled == FALSE. Uses cached SQLite
 * vectors in memory first (only reading vault files when uncached and vault != NULL). */
GPtrArray *bt_embed_hybrid_search(const BtVault *vault, BtIndex *index, const gchar *query,
                                  guint limit, gboolean enabled, GError **error);

/* Returns count of persisted note embedding vectors currently stored in SQLite. */
guint bt_embed_count_cached(BtIndex *index);

G_END_DECLS

#endif
