/* index.h — SQLite L1 derived cache + FTS5 + ADR-007 single-writer thread (§7.2, §7.3).
 *
 * All SQLite operations execute exclusively on the single writer thread owned
 * by BtIndex. Every durable fact lives in L0 .md files; this DB is disposable
 * and deterministically rebuildable (P5). */
#ifndef BT_INDEX_H
#define BT_INDEX_H

#include <glib.h>

#include "bterror.h"
#include "vault.h"

G_BEGIN_DECLS

#define BT_SCHEMA_VERSION 2

typedef struct BtIndex BtIndex;

typedef struct {
  gchar *note_id;
  gchar *path;
  gchar *title;
  gchar *snippet;
  gdouble rank;
} BtSearchHit;

typedef struct {
  gchar *id;
  gchar *path;
  gchar *title;
  gint64 created;
  gint64 updated;
  gchar *source;
  gchar *agent;
  gchar *fm_hash;
  gchar *body_hash;
  gboolean parse_error;
} BtNoteRow;

typedef struct {
  gchar *tag;
  gchar *origin;     /* "human" | "ai" */
  gdouble confidence;
  gboolean has_confidence;
} BtTagRow;

typedef struct {
  gchar *src;
  gchar *dst;
  gchar *kind;       /* "link" | "similar" | "temporal" | "tagged" */
  gdouble weight;
} BtEdgeRow;

typedef struct {
  gchar *src_note_id;
  gchar *src_path;
  gchar *raw_target;
  gchar *resolved_path; /* NULL when unresolved/external/blocked */
  gchar *status;        /* "resolved" | "ambiguous" | "unresolved" | "external" | "blocked" */
  gchar *kind;          /* "note" | "file" | "dir" | "url" | "same_note" */
  gint line_no;
} BtLinkResRow;

void bt_search_hit_free(gpointer hit);
void bt_note_row_free(gpointer row);
void bt_tag_row_free(gpointer row);
void bt_edge_row_free(gpointer row);
void bt_link_res_row_free(gpointer row);

/* Opens vault/.bt/index.db on a dedicated single-writer thread (ADR-007).
 * Checks PRAGMA integrity_check (R2: if corrupt, deletes DB and sets
 * *rebuilt_r2 = TRUE) and runs migrations up to BT_SCHEMA_VERSION.
 * Refuses any DB with user_version > BT_SCHEMA_VERSION. */
BtIndex *bt_index_open(const gchar *vault_root, gboolean *rebuilt_r2, GError **error);
BtIndex *bt_index_open_readonly(const gchar *db_path, GError **error);
void bt_index_close(BtIndex *idx);

gint bt_index_user_version(BtIndex *idx);
guint bt_index_note_count(BtIndex *idx);

/* Reconciles a single note file -> index (§7.3). No-ops if fm_hash & body_hash
 * match (set *skipped_unchanged = TRUE if non-NULL). */
gboolean bt_index_upsert_note(BtIndex *idx, const BtVault *vault, const gchar *relpath,
                              gboolean *skipped_unchanged, GError **error);

gboolean bt_index_delete_path(BtIndex *idx, const gchar *relpath, GError **error);

/* P5 full rebuild from L0 in deterministic path order. */
gboolean bt_index_rebuild(BtIndex *idx, const BtVault *vault, GError **error);

/* FTS5 search with bm25 ranking and snippet(). */
GPtrArray *bt_index_search_fts(BtIndex *idx, const gchar *query, guint limit, GError **error);

/* Deterministic canonical text dump of notes/tags/edges/notes_fts (P5 golden test). */
gchar *bt_index_dump_canonical(BtIndex *idx, GError **error);

/* Imports M3 vault/.bt/sessions.json into SQLite sessions table. */
gboolean bt_index_import_sessions_json(BtIndex *idx, const gchar *vault_root, GError **error);

/* Session table operations (M4/M5/M7/M8). */
gboolean bt_index_session_upsert(BtIndex *idx, const gchar *id, const gchar *tool,
                                 const gchar *cwd, gint64 started, gint64 ended,
                                 const gchar *resume_cmd, const gchar *note_id, GError **error);
gboolean bt_index_session_end(BtIndex *idx, const gchar *id, gint64 ended, GError **error);

/* Note / tag / edge / embedding query helpers used by M5–M8 & bt-mcp. */
GPtrArray *bt_index_list_notes(BtIndex *idx, GError **error);
GPtrArray *bt_index_list_tags(BtIndex *idx, const gchar *note_id, GError **error);
GPtrArray *bt_index_list_edges(BtIndex *idx, GError **error);
GPtrArray *bt_index_list_link_resolutions(BtIndex *idx, const gchar *src_note_id, GError **error);
gchar *bt_index_get_body_hash(BtIndex *idx, const gchar *relpath);

/* Tag governance helpers (M6 human lock). */
gboolean bt_index_set_tag(BtIndex *idx, const gchar *note_id, const gchar *tag,
                          const gchar *origin, gdouble confidence, gboolean has_conf,
                          GError **error);
gboolean bt_index_delete_tag(BtIndex *idx, const gchar *note_id, const gchar *tag,
                             const gchar *origin, GError **error);

/* Embedding storage helpers (M8 optional). */
gboolean bt_index_put_embedding(BtIndex *idx, const gchar *note_id, gint chunk,
                                const gfloat *vec, gsize dim, GError **error);
gboolean bt_index_clear_embeddings(BtIndex *idx, GError **error);
gboolean bt_index_put_edge(BtIndex *idx, const gchar *src, const gchar *dst, const gchar *kind,
                           gdouble weight, GError **error);

/* Direct SQL execution helper for tests (read-only assertions). */
gint bt_index_count_fts_matches(BtIndex *idx, const gchar *query);

G_END_DECLS

#endif
