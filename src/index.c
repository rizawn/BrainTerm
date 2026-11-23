/* index.c — SQLite L1 cache + FTS5 triggers + ADR-007 single-writer thread. */
#include "index.h"
#include "frontmatter.h"
#include "linkresolve.h"
#include "mdparse.h"
#include "sessions.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

/* Embedded migrations/0001_init.sql (§7.2 verbatim + body column for FTS5). */
static const char *const MIGRATION_0001_SQL =
    "CREATE TABLE notes(\n"
    "  id TEXT PRIMARY KEY,\n"
    "  path TEXT UNIQUE NOT NULL,\n"
    "  title TEXT NOT NULL DEFAULT '',\n"
    "  body TEXT NOT NULL DEFAULT '',\n"
    "  created INTEGER NOT NULL,\n"
    "  updated INTEGER NOT NULL,\n"
    "  source TEXT NOT NULL CHECK(source IN ('human','ai','import')),\n"
    "  agent TEXT,\n"
    "  fm_hash TEXT NOT NULL,\n"
    "  body_hash TEXT NOT NULL,\n"
    "  parse_error INTEGER NOT NULL DEFAULT 0\n"
    ");\n"
    "CREATE TABLE tags(\n"
    "  note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,\n"
    "  tag TEXT NOT NULL,\n"
    "  origin TEXT NOT NULL CHECK(origin IN ('human','ai')),\n"
    "  confidence REAL,\n"
    "  PRIMARY KEY(note_id, tag, origin)\n"
    ");\n"
    "CREATE INDEX idx_tags_tag ON tags(tag);\n"
    "CREATE TABLE edges(\n"
    "  src TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,\n"
    "  dst TEXT NOT NULL,\n"
    "  kind TEXT NOT NULL CHECK(kind IN ('link','similar','temporal','tagged')),\n"
    "  weight REAL NOT NULL DEFAULT 1.0,\n"
    "  PRIMARY KEY(src, dst, kind)\n"
    ");\n"
    "CREATE INDEX idx_edges_dst ON edges(dst, kind);\n"
    "CREATE TABLE sessions(\n"
    "  id TEXT PRIMARY KEY,\n"
    "  tool TEXT NOT NULL,\n"
    "  cwd TEXT NOT NULL,\n"
    "  started INTEGER NOT NULL,\n"
    "  ended INTEGER,\n"
    "  resume_cmd TEXT,\n"
    "  note_id TEXT REFERENCES notes(id) ON DELETE SET NULL\n"
    ");\n"
    "CREATE TABLE embeddings(\n"
    "  note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,\n"
    "  chunk INTEGER NOT NULL,\n"
    "  vec BLOB NOT NULL,\n"
    "  PRIMARY KEY(note_id, chunk)\n"
    ");\n"
    "CREATE VIRTUAL TABLE notes_fts USING fts5(\n"
    "  title, body, content='notes', content_rowid='rowid', tokenize='unicode61'\n"
    ");\n"
    "CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN\n"
    "  INSERT INTO notes_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);\n"
    "END;\n"
    "CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN\n"
    "  INSERT INTO notes_fts(notes_fts, rowid, title, body) VALUES('delete', old.rowid, old.title, old.body);\n"
    "END;\n"
    "CREATE TRIGGER notes_au AFTER UPDATE ON notes BEGIN\n"
    "  INSERT INTO notes_fts(notes_fts, rowid, title, body) VALUES('delete', old.rowid, old.title, old.body);\n"
    "  INSERT INTO notes_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);\n"
    "END;\n";

/* Embedded migrations/0002_links_resolution.sql. */
static const char *const MIGRATION_0002_SQL =
    "CREATE TABLE link_resolutions(\n"
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "  src_note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,\n"
    "  src_path TEXT NOT NULL,\n"
    "  raw_target TEXT NOT NULL,\n"
    "  resolved_path TEXT,\n"
    "  status TEXT NOT NULL CHECK(status IN ('resolved','ambiguous','unresolved','external','blocked')),\n"
    "  kind TEXT NOT NULL CHECK(kind IN ('note','file','dir','url','same_note')),\n"
    "  line_no INTEGER NOT NULL DEFAULT 1\n"
    ");\n"
    "CREATE INDEX idx_link_res_src ON link_resolutions(src_note_id);\n"
    "CREATE INDEX idx_link_res_status ON link_resolutions(status);\n"
    "CREATE INDEX idx_link_res_resolved ON link_resolutions(resolved_path);\n";

void bt_search_hit_free(gpointer hit)
{
  BtSearchHit *h = hit;
  if (!h) return;
  g_free(h->note_id);
  g_free(h->path);
  g_free(h->title);
  g_free(h->snippet);
  g_free(h);
}

void bt_note_row_free(gpointer row)
{
  BtNoteRow *r = row;
  if (!r) return;
  g_free(r->id);
  g_free(r->path);
  g_free(r->title);
  g_free(r->source);
  g_free(r->agent);
  g_free(r->fm_hash);
  g_free(r->body_hash);
  g_free(r);
}

void bt_tag_row_free(gpointer row)
{
  BtTagRow *r = row;
  if (!r) return;
  g_free(r->tag);
  g_free(r->origin);
  g_free(r);
}

void bt_edge_row_free(gpointer row)
{
  BtEdgeRow *r = row;
  if (!r) return;
  g_free(r->src);
  g_free(r->dst);
  g_free(r->kind);
  g_free(r);
}

void bt_link_res_row_free(gpointer row)
{
  BtLinkResRow *r = row;
  if (!r) return;
  g_free(r->src_note_id);
  g_free(r->src_path);
  g_free(r->raw_target);
  g_free(r->resolved_path);
  g_free(r->status);
  g_free(r->kind);
  g_free(r);
}

/* ------------------------------------------- ADR-007 single-writer queue */

typedef void (*DbJobFn)(sqlite3 *db, gpointer ctx);

typedef struct {
  DbJobFn fn;
  gpointer ctx;
  gboolean quit;
  GMutex done_lock;
  GCond done_cond;
  gboolean done;
} DbJob;

struct BtIndex {
  gchar *db_path;
  gboolean readonly;
  sqlite3 *db; /* owned EXCLUSIVELY by writer_thread */
  GThread *writer_thread;
  GAsyncQueue *queue;
};

static gpointer writer_thread_main(gpointer ud)
{
  BtIndex *idx = ud;
  for (;;) {
    DbJob *job = g_async_queue_pop(idx->queue);
    if (job->quit) {
      g_mutex_lock(&job->done_lock);
      job->done = TRUE;
      g_cond_signal(&job->done_cond);
      g_mutex_unlock(&job->done_lock);
      break;
    }
    if (job->fn != NULL)
      job->fn(idx->db, job->ctx);
    g_mutex_lock(&job->done_lock);
    job->done = TRUE;
    g_cond_signal(&job->done_cond);
    g_mutex_unlock(&job->done_lock);
  }
  return NULL;
}

static void run_on_writer(BtIndex *idx, DbJobFn fn, gpointer ctx)
{
  DbJob job;
  memset(&job, 0, sizeof(job));
  job.fn = fn;
  job.ctx = ctx;
  g_mutex_init(&job.done_lock);
  g_cond_init(&job.done_cond);
  g_async_queue_push(idx->queue, &job);

  g_mutex_lock(&job.done_lock);
  while (!job.done)
    g_cond_wait(&job.done_cond, &job.done_lock);
  g_mutex_unlock(&job.done_lock);

  g_mutex_clear(&job.done_lock);
  g_cond_clear(&job.done_cond);
}

static gboolean exec_sql(sqlite3 *db, const char *sql, GError **error)
{
  char *emsg = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &emsg);
  if (rc != SQLITE_OK) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "SQLite error: %s", emsg ? emsg : sqlite3_errmsg(db));
    sqlite3_free(emsg);
    return FALSE;
  }
  return TRUE;
}

static int query_pragma_int(sqlite3 *db, const char *pragma)
{
  sqlite3_stmt *st = NULL;
  int val = 0;
  if (sqlite3_prepare_v2(db, pragma, -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW)
      val = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  return val;
}

static gboolean check_integrity(sqlite3 *db)
{
  sqlite3_stmt *st = NULL;
  gboolean ok = FALSE;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *txt = sqlite3_column_text(st, 0);
      ok = txt != NULL && strcmp((const char *)txt, "ok") == 0;
    }
    sqlite3_finalize(st);
  }
  return ok;
}

typedef struct {
  BtIndex *idx;
  gboolean *rebuilt_r2;
  GError **error;
  gboolean ok;
} OpenCtx;

static void job_open(sqlite3 *unused, gpointer ud)
{
  (void)unused;
  OpenCtx *c = ud;
  BtIndex *idx = c->idx;
  if (c->rebuilt_r2)
    *c->rebuilt_r2 = FALSE;

  int flags = idx->readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  int rc = sqlite3_open_v2(idx->db_path, &idx->db, flags, NULL);
  if (rc != SQLITE_OK || (!idx->readonly && !check_integrity(idx->db))) {
    if (idx->db != NULL) {
      sqlite3_close(idx->db);
      idx->db = NULL;
    }
    if (idx->readonly) {
      g_set_error(c->error, BT_ERROR, BT_ERROR_IO, "Cannot open read-only index '%s'", idx->db_path);
      return;
    }
    /* R2: corrupted DB -> delete and recreate cleanly. */
    g_remove(idx->db_path);
    gchar *wal = g_strconcat(idx->db_path, "-wal", NULL);
    gchar *shm = g_strconcat(idx->db_path, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_free(wal);
    g_free(shm);
    if (c->rebuilt_r2)
      *c->rebuilt_r2 = TRUE;
    rc = sqlite3_open_v2(idx->db_path, &idx->db, flags, NULL);
    if (rc != SQLITE_OK) {
      g_set_error(c->error, BT_ERROR, BT_ERROR_IO, "Cannot create index '%s': %s", idx->db_path,
                  sqlite3_errmsg(idx->db));
      return;
    }
  }

  sqlite3_busy_timeout(idx->db, 3000);
  if (!idx->readonly) {
    if (!exec_sql(idx->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;",
                  c->error))
      return;
  }

  int ver = query_pragma_int(idx->db, "PRAGMA user_version;");
  if (ver > BT_SCHEMA_VERSION) {
    g_set_error(c->error, BT_ERROR, BT_ERROR_INVALID,
                "Index schema version %d is newer than supported version %d (downgrade refused)",
                ver, BT_SCHEMA_VERSION);
    sqlite3_close(idx->db);
    idx->db = NULL;
    return;
  }
  if (!idx->readonly && ver < BT_SCHEMA_VERSION) {
    if (!exec_sql(idx->db, "BEGIN IMMEDIATE;", c->error))
      return;
    if (ver < 1) {
      if (!exec_sql(idx->db, MIGRATION_0001_SQL, c->error)) {
        sqlite3_exec(idx->db, "ROLLBACK;", NULL, NULL, NULL);
        return;
      }
    }
    if (ver < 2) {
      if (!exec_sql(idx->db, MIGRATION_0002_SQL, c->error)) {
        sqlite3_exec(idx->db, "ROLLBACK;", NULL, NULL, NULL);
        return;
      }
    }
    if (!exec_sql(idx->db, "PRAGMA user_version=2; COMMIT;", c->error)) {
      sqlite3_exec(idx->db, "ROLLBACK;", NULL, NULL, NULL);
      return;
    }
  }
  c->ok = TRUE;
}

static BtIndex *open_common(const gchar *db_path, gboolean readonly, gboolean *rebuilt_r2,
                            GError **error)
{
  if (!readonly) {
    gchar *dir = g_path_get_dirname(db_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }
  BtIndex *idx = g_new0(BtIndex, 1);
  idx->db_path = g_strdup(db_path);
  idx->readonly = readonly;
  idx->queue = g_async_queue_new();
  idx->writer_thread = g_thread_new("bt-db-writer", writer_thread_main, idx);

  OpenCtx oc = {.idx = idx, .rebuilt_r2 = rebuilt_r2, .error = error, .ok = FALSE};
  run_on_writer(idx, job_open, &oc);
  if (!oc.ok) {
    bt_index_close(idx);
    return NULL;
  }
  return idx;
}

BtIndex *bt_index_open(const gchar *vault_root, gboolean *rebuilt_r2, GError **error)
{
  g_return_val_if_fail(vault_root != NULL, NULL);
  gchar *db_path = g_build_filename(vault_root, ".bt", "index.db", NULL);
  BtIndex *idx = open_common(db_path, FALSE, rebuilt_r2, error);
  g_free(db_path);
  return idx;
}

BtIndex *bt_index_open_readonly(const gchar *db_path, GError **error)
{
  g_return_val_if_fail(db_path != NULL, NULL);
  return open_common(db_path, TRUE, NULL, error);
}

static void job_close(sqlite3 *db, gpointer ud)
{
  BtIndex *idx = ud;
  if (db != NULL) {
    sqlite3_close(db);
    idx->db = NULL;
  }
}

void bt_index_close(BtIndex *idx)
{
  if (idx == NULL)
    return;
  run_on_writer(idx, job_close, idx);

  DbJob quit_job;
  memset(&quit_job, 0, sizeof(quit_job));
  quit_job.quit = TRUE;
  g_mutex_init(&quit_job.done_lock);
  g_cond_init(&quit_job.done_cond);
  g_async_queue_push(idx->queue, &quit_job);
  g_mutex_lock(&quit_job.done_lock);
  while (!quit_job.done)
    g_cond_wait(&quit_job.done_cond, &quit_job.done_lock);
  g_mutex_unlock(&quit_job.done_lock);
  g_mutex_clear(&quit_job.done_lock);
  g_cond_clear(&quit_job.done_cond);

  g_thread_join(idx->writer_thread);
  g_async_queue_unref(idx->queue);
  g_free(idx->db_path);
  g_free(idx);
}

/* ----------------------------------------------------- note upsert (§7.3) */

static gchar *fallback_title_from_path(const gchar *relpath)
{
  gchar *base = g_path_get_basename(relpath);
  gsize n = strlen(base);
  if (n > 3 && g_ascii_strcasecmp(base + n - 3, ".md") == 0)
    base[n - 3] = '\0';
  return base;
}

static gchar *resolve_unique_id(sqlite3 *db, const gchar *desired, const gchar *relpath)
{
  gchar *cand = g_strdup(desired);
  for (guint suffix = 1;; suffix++) {
    sqlite3_stmt *st = NULL;
    gboolean collision = FALSE;
    if (sqlite3_prepare_v2(db, "SELECT path FROM notes WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, cand, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char *existing_path = (const char *)sqlite3_column_text(st, 0);
        if (existing_path != NULL && strcmp(existing_path, relpath) != 0)
          collision = TRUE;
      }
      sqlite3_finalize(st);
    }
    if (!collision)
      return cand;
    g_free(cand);
    cand = g_strdup_printf("%s-%u", desired, suffix + 1);
  }
}

typedef struct {
  const BtVault *vault;
  const gchar *relpath;
  gchar *raw;
  gsize raw_len;
  gboolean *skipped;
  GError **error;
  gboolean ok;
} UpsertCtx;

static gboolean upsert_in_db(sqlite3 *db, const gchar *relpath, const gchar *raw, gsize raw_len,
                             gboolean *skipped, GError **error)
{
  BtFrontmatter *fm = bt_frontmatter_parse(raw, raw_len);
  const gchar *body_ptr = raw + fm->body_offset;
  gsize body_len = (raw_len >= fm->body_offset) ? (raw_len - fm->body_offset) : 0;

  gchar *fm_hash = bt_fnv1a_hex(raw, fm->body_offset);
  gchar *body_hash = bt_fnv1a_hex(body_ptr, body_len);

  /* Fast no-op check via fm_hash + body_hash (§7.3). */
  sqlite3_stmt *chk = NULL;
  if (sqlite3_prepare_v2(db, "SELECT fm_hash, body_hash FROM notes WHERE path=?1;", -1, &chk, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_text(chk, 1, relpath, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(chk) == SQLITE_ROW) {
      const char *ef = (const char *)sqlite3_column_text(chk, 0);
      const char *eb = (const char *)sqlite3_column_text(chk, 1);
      if (ef && eb && strcmp(ef, fm_hash) == 0 && strcmp(eb, body_hash) == 0) {
        sqlite3_finalize(chk);
        if (skipped) *skipped = TRUE;
        g_free(fm_hash);
        g_free(body_hash);
        bt_frontmatter_free(fm);
        return TRUE;
      }
    }
    sqlite3_finalize(chk);
  }
  if (skipped) *skipped = FALSE;

  BtMdExtract *ex = bt_mdparse_extract(body_ptr, body_len);

  /* Determine canonical id (deterministic from path if missing). */
  gchar *base_id = NULL;
  if (fm->id != NULL && fm->id[0] != '\0') {
    base_id = g_strdup(fm->id);
  } else {
    gchar *ph = bt_fnv1a_hex(relpath, strlen(relpath));
    base_id = g_strdup_printf("bt-auto-%.8s", ph);
    g_free(ph);
  }
  gchar *note_id = resolve_unique_id(db, base_id, relpath);
  g_free(base_id);

  gchar *title = fm->title ? g_strdup(fm->title)
                           : (ex->first_h1 ? g_strdup(ex->first_h1)
                                           : fallback_title_from_path(relpath));
  gint64 created = fm->created_ms;
  gint64 updated = fm->updated_ms ? fm->updated_ms : created;

  /* Delete old row at same path if id changed. */
  sqlite3_stmt *del = NULL;
  if (sqlite3_prepare_v2(db, "DELETE FROM notes WHERE path=?1 AND id!=?2;", -1, &del, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_text(del, 1, relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(del, 2, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);
  }

  /* Upsert notes row (triggers notes_ai / notes_au keep notes_fts in sync!). */
  sqlite3_stmt *ins = NULL;
  const char *sql =
      "INSERT INTO notes(id, path, title, body, created, updated, source, agent, fm_hash, body_hash, parse_error) "
      "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) "
      "ON CONFLICT(id) DO UPDATE SET "
      "  path=excluded.path, title=excluded.title, body=excluded.body, "
      "  created=excluded.created, updated=excluded.updated, source=excluded.source, "
      "  agent=excluded.agent, fm_hash=excluded.fm_hash, body_hash=excluded.body_hash, "
      "  parse_error=excluded.parse_error;";
  if (sqlite3_prepare_v2(db, sql, -1, &ins, NULL) != SQLITE_OK) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "SQLite prepare failed: %s", sqlite3_errmsg(db));
    goto fail;
  }
  sqlite3_bind_text(ins, 1, note_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 2, relpath, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 3, title, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 4, ex->plain_body, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(ins, 5, created);
  sqlite3_bind_int64(ins, 6, updated);
  sqlite3_bind_text(ins, 7, fm->source ? fm->source : "human", -1, SQLITE_TRANSIENT);
  if (fm->agent)
    sqlite3_bind_text(ins, 8, fm->agent, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(ins, 8);
  sqlite3_bind_text(ins, 9, fm_hash, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 10, body_hash, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(ins, 11, fm->parse_error ? 1 : 0);
  int rc = sqlite3_step(ins);
  sqlite3_finalize(ins);
  if (rc != SQLITE_DONE) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "SQLite upsert failed: %s", sqlite3_errmsg(db));
    goto fail;
  }

  /* Rebuild derived tags and link/tagged edges for this note from L0 truth. */
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "DELETE FROM tags WHERE note_id=?1;", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  if (sqlite3_prepare_v2(db, "DELETE FROM edges WHERE src=?1 AND kind IN ('link','tagged');", -1,
                         &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  if (sqlite3_prepare_v2(db, "DELETE FROM link_resolutions WHERE src_note_id=?1;", -1, &st, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_text(st, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }

  sqlite3_stmt *tins = NULL;
  sqlite3_stmt *eins = NULL;
  sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO tags(note_id, tag, origin, confidence) VALUES(?1,?2,?3,?4);",
                     -1, &tins, NULL);
  sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO edges(src, dst, kind, weight) VALUES(?1,?2,?3,?4);",
                     -1, &eins, NULL);

  for (guint i = 0; i < fm->tags->len; i++) {
    const gchar *tg = g_ptr_array_index(fm->tags, i);
    sqlite3_reset(tins);
    sqlite3_bind_text(tins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tins, 2, tg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tins, 3, "human", -1, SQLITE_STATIC);
    sqlite3_bind_null(tins, 4);
    sqlite3_step(tins);

    sqlite3_reset(eins);
    sqlite3_bind_text(eins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 2, tg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 3, "tagged", -1, SQLITE_STATIC);
    sqlite3_bind_double(eins, 4, 1.0);
    sqlite3_step(eins);
  }
  for (guint i = 0; i < ex->tags->len; i++) {
    const gchar *tg = g_ptr_array_index(ex->tags, i);
    sqlite3_reset(tins);
    sqlite3_bind_text(tins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tins, 2, tg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tins, 3, "human", -1, SQLITE_STATIC);
    sqlite3_bind_null(tins, 4);
    sqlite3_step(tins);

    sqlite3_reset(eins);
    sqlite3_bind_text(eins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 2, tg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 3, "tagged", -1, SQLITE_STATIC);
    sqlite3_bind_double(eins, 4, 1.0);
    sqlite3_step(eins);
  }
  for (guint i = 0; i < fm->tags_ai->len; i++) {
    const gchar *tg = g_ptr_array_index(fm->tags_ai, i);
    sqlite3_reset(tins);
    sqlite3_bind_text(tins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tins, 2, tg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tins, 3, "ai", -1, SQLITE_STATIC);
    sqlite3_bind_double(tins, 4, 0.75);
    sqlite3_step(tins);

    sqlite3_reset(eins);
    sqlite3_bind_text(eins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 2, tg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 3, "tagged", -1, SQLITE_STATIC);
    sqlite3_bind_double(eins, 4, 0.75);
    sqlite3_step(eins);
  }

  /* Link edges from frontmatter links + shared link scanner (excludes code & math spans). */
  for (guint i = 0; i < fm->links->len; i++) {
    const gchar *lnk = g_ptr_array_index(fm->links, i);
    gchar *clean = g_strdup(lnk);
    if (g_str_has_prefix(clean, "[[") && g_str_has_suffix(clean, "]]") && strlen(clean) > 4) {
      gchar *inner = g_strndup(clean + 2, strlen(clean) - 4);
      g_free(clean);
      clean = inner;
    }
    sqlite3_reset(eins);
    sqlite3_bind_text(eins, 1, note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 2, clean, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(eins, 3, "link", -1, SQLITE_STATIC);
    sqlite3_bind_double(eins, 4, 1.0);
    sqlite3_step(eins);
    g_free(clean);
  }

  GArray *lspans = bt_link_scan(raw, raw_len);
  sqlite3_stmt *lr_ins = NULL;
  sqlite3_prepare_v2(
      db,
      "INSERT INTO link_resolutions(src_note_id, src_path, raw_target, resolved_path, status, kind, line_no) "
      "VALUES(?1,?2,?3,NULL,'unresolved','note',?4);",
      -1, &lr_ins, NULL);
  for (guint i = 0; i < lspans->len; i++) {
    const BtLinkSpan *sp = &g_array_index(lspans, BtLinkSpan, i);
    if (lr_ins != NULL) {
      sqlite3_reset(lr_ins);
      sqlite3_bind_text(lr_ins, 1, note_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(lr_ins, 2, relpath, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(lr_ins, 3, sp->raw_target, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(lr_ins, 4, (int)sp->line_no);
      sqlite3_step(lr_ins);
    }
    if (sp->syntax == BT_LINK_SYNTAX_WIKILINK ||
        (sp->syntax == BT_LINK_SYNTAX_MARKDOWN &&
         g_ascii_strncasecmp(sp->raw_target, "http://", 7) != 0 &&
         g_ascii_strncasecmp(sp->raw_target, "https://", 8) != 0 &&
         g_ascii_strncasecmp(sp->raw_target, "mailto:", 7) != 0)) {
      sqlite3_reset(eins);
      sqlite3_bind_text(eins, 1, note_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(eins, 2, sp->raw_target, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(eins, 3, "link", -1, SQLITE_STATIC);
      sqlite3_bind_double(eins, 4, 1.0);
      sqlite3_step(eins);
    }
  }
  sqlite3_finalize(lr_ins);
  g_array_unref(lspans);

  sqlite3_finalize(tins);
  sqlite3_finalize(eins);
  g_free(title);
  g_free(note_id);
  bt_mdparse_free(ex);
  g_free(fm_hash);
  g_free(body_hash);
  bt_frontmatter_free(fm);
  return TRUE;

fail:
  g_free(title);
  g_free(note_id);
  bt_mdparse_free(ex);
  g_free(fm_hash);
  g_free(body_hash);
  bt_frontmatter_free(fm);
  return FALSE;
}

static BtIndexSnapshot *snapshot_from_db_notes(sqlite3 *db)
{
  BtIndexSnapshot *snap = bt_index_snapshot_new();
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT path, title FROM notes;", -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *p = (const char *)sqlite3_column_text(st, 0);
      const char *t = (const char *)sqlite3_column_text(st, 1);
      if (p != NULL && p[0] != '\0')
        bt_index_snapshot_add_entry(snap, p, FALSE, (t && t[0]) ? t : NULL, NULL);
    }
    sqlite3_finalize(st);
  }
  bt_index_snapshot_finalize(snap);
  return snap;
}

static void re_resolve_all_in_db(sqlite3 *db, const BtVault *vault)
{
  BtIndexSnapshot *snap = vault ? bt_index_snapshot_from_vault(vault) : snapshot_from_db_notes(db);
  if (snap == NULL)
    return;

  typedef struct {
    sqlite3_int64 rowid;
    gchar *src_path;
    gchar *raw_target;
  } PendingLR;
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);

  sqlite3_stmt *sel = NULL;
  if (sqlite3_prepare_v2(db, "SELECT id, src_path, raw_target FROM link_resolutions;", -1, &sel, NULL) ==
      SQLITE_OK) {
    while (sqlite3_step(sel) == SQLITE_ROW) {
      PendingLR *p = g_new0(PendingLR, 1);
      p->rowid = sqlite3_column_int64(sel, 0);
      const char *sp = (const char *)sqlite3_column_text(sel, 1);
      const char *rt = (const char *)sqlite3_column_text(sel, 2);
      p->src_path = g_strdup(sp ? sp : "");
      p->raw_target = g_strdup(rt ? rt : "");
      g_ptr_array_add(rows, p);
    }
    sqlite3_finalize(sel);
  }

  sqlite3_stmt *upd = NULL;
  sqlite3_stmt *del = NULL;
  sqlite3_prepare_v2(db, "DELETE FROM link_resolutions WHERE id=?1;", -1, &del, NULL);
  if (sqlite3_prepare_v2(
          db, "UPDATE link_resolutions SET resolved_path=?1, status=?2, kind=?3 WHERE id=?4;",
          -1, &upd, NULL) == SQLITE_OK) {
    for (guint i = 0; i < rows->len; i++) {
      PendingLR *p = g_ptr_array_index(rows, i);
      BtLinkResolution *res = bt_link_resolve(snap, p->src_path, p->raw_target);
      if (res != NULL && res->status == BT_LINK_PLACEHOLDER) {
        if (del != NULL) {
          sqlite3_reset(del);
          sqlite3_bind_int64(del, 1, p->rowid);
          sqlite3_step(del);
        }
        bt_link_resolution_free(res);
        g_free(p->src_path);
        g_free(p->raw_target);
        continue;
      }
      const char *status_str = res ? bt_link_status_name(res->status) : "unresolved";
      const char *kind_str = res ? bt_link_kind_name(res->kind) : "note";
      sqlite3_reset(upd);
      if (res != NULL && res->path != NULL)
        sqlite3_bind_text(upd, 1, res->path, -1, SQLITE_TRANSIENT);
      else
        sqlite3_bind_null(upd, 1);
      sqlite3_bind_text(upd, 2, status_str, -1, SQLITE_STATIC);
      sqlite3_bind_text(upd, 3, kind_str, -1, SQLITE_STATIC);
      sqlite3_bind_int64(upd, 4, p->rowid);
      sqlite3_step(upd);
      bt_link_resolution_free(res);
      g_free(p->src_path);
      g_free(p->raw_target);
    }
    sqlite3_finalize(upd);
  } else {
    for (guint i = 0; i < rows->len; i++) {
      PendingLR *p = g_ptr_array_index(rows, i);
      g_free(p->src_path);
      g_free(p->raw_target);
    }
  }
  sqlite3_finalize(del);
  g_ptr_array_unref(rows);
  bt_index_snapshot_free(snap);
}

static void job_upsert(sqlite3 *db, gpointer ud)
{
  UpsertCtx *c = ud;
  if (!exec_sql(db, "BEGIN IMMEDIATE;", c->error))
    return;
  c->ok = upsert_in_db(db, c->relpath, c->raw, c->raw_len, c->skipped, c->error);
  if (c->ok && !(c->skipped != NULL && *c->skipped))
    re_resolve_all_in_db(db, c->vault);
  if (c->ok)
    c->ok = exec_sql(db, "COMMIT;", c->error);
  else
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
}

gboolean bt_index_upsert_note(BtIndex *idx, const BtVault *vault, const gchar *relpath,
                              gboolean *skipped_unchanged, GError **error)
{
  g_return_val_if_fail(idx != NULL && vault != NULL && relpath != NULL, FALSE);
  gchar *raw = NULL;
  gsize len = 0;
  if (!vault_read(vault, relpath, &raw, &len, error))
    return FALSE;
  UpsertCtx c = {
    .vault = vault, .relpath = relpath, .raw = raw, .raw_len = len,
    .skipped = skipped_unchanged, .error = error, .ok = FALSE,
  };
  run_on_writer(idx, job_upsert, &c);
  g_free(raw);
  return c.ok;
}

static gboolean target_may_match_created(const char *raw_target, const char *status_txt,
                                         const char *new_rel, const char *new_stem)
{
  if (status_txt == NULL || strcmp(status_txt, "resolved") != 0)
    return TRUE;
  if (raw_target == NULL || raw_target[0] == '\0')
    return FALSE;
  /* Strip optional |alias and #anchor or ^block to inspect stem/basename match. */
  g_autofree gchar *clean = g_strdup(raw_target);
  char *bar = strchr(clean, '|');
  if (bar != NULL) *bar = '\0';
  char *hash = strchr(clean, '#');
  if (hash != NULL) *hash = '\0';
  char *caret = strchr(clean, '^');
  if (caret != NULL) *caret = '\0';
  gsize clen = strlen(clean);
  if (clen > 3 && g_ascii_strcasecmp(clean + clen - 3, ".md") == 0)
    clean[clen - 3] = '\0';
  else if (clen > 9 && g_ascii_strcasecmp(clean + clen - 9, ".markdown") == 0)
    clean[clen - 9] = '\0';
  const char *slash = strrchr(clean, '/');
  const char *cand_stem = slash ? (slash + 1) : clean;
  if (new_stem != NULL && g_ascii_strcasecmp(cand_stem, new_stem) == 0)
    return TRUE;
  if (new_rel != NULL && g_ascii_strcasecmp(clean, new_rel) == 0)
    return TRUE;
  return FALSE;
}

static void job_upsert_created(sqlite3 *db, gpointer ud)
{
  UpsertCtx *c = ud;
  if (!exec_sql(db, "BEGIN IMMEDIATE;", c->error))
    return;
  c->ok = upsert_in_db(db, c->relpath, c->raw, c->raw_len, c->skipped, c->error);
  if (!c->ok) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return;
  }

  BtIndexSnapshot *snap = snapshot_from_db_notes(db);
  if (snap != NULL) {
    if (c->vault != NULL)
      bt_index_snapshot_set_vault(snap, c->vault);

    const char *base_ptr = strrchr(c->relpath, '/');
    base_ptr = base_ptr ? (base_ptr + 1) : c->relpath;
    g_autofree gchar *new_stem = g_strdup(base_ptr);
    gsize nlen = strlen(new_stem);
    if (nlen > 3 && g_ascii_strcasecmp(new_stem + nlen - 3, ".md") == 0)
      new_stem[nlen - 3] = '\0';
    else if (nlen > 9 && g_ascii_strcasecmp(new_stem + nlen - 9, ".markdown") == 0)
      new_stem[nlen - 9] = '\0';

    typedef struct {
      sqlite3_int64 rowid;
      gchar *src_path;
      gchar *raw_target;
    } TargetRow;
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, src_path, raw_target, status FROM link_resolutions;",
                           -1, &sel, NULL) == SQLITE_OK) {
      while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *st_txt = (const char *)sqlite3_column_text(sel, 3);
        const char *rt = (const char *)sqlite3_column_text(sel, 2);
        if (!target_may_match_created(rt, st_txt, c->relpath, new_stem))
          continue;
        TargetRow *r = g_new0(TargetRow, 1);
        r->rowid = sqlite3_column_int64(sel, 0);
        r->src_path = g_strdup((const char *)sqlite3_column_text(sel, 1));
        r->raw_target = g_strdup(rt);
        g_ptr_array_add(rows, r);
      }
      sqlite3_finalize(sel);
    }

    sqlite3_stmt *del = NULL;
    sqlite3_stmt *upd = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM link_resolutions WHERE id=?1;", -1, &del, NULL);
    sqlite3_prepare_v2(
        db, "UPDATE link_resolutions SET resolved_path=?1, status=?2, kind=?3 WHERE id=?4;",
        -1, &upd, NULL);
    for (guint i = 0; i < rows->len; i++) {
      TargetRow *r = g_ptr_array_index(rows, i);
      BtLinkResolution *res = bt_link_resolve(snap, r->src_path, r->raw_target);
      if (res == NULL) {
        if (del != NULL) {
          sqlite3_reset(del);
          sqlite3_clear_bindings(del);
          sqlite3_bind_int64(del, 1, r->rowid);
          sqlite3_step(del);
        }
      } else {
        if (upd != NULL) {
          sqlite3_reset(upd);
          sqlite3_clear_bindings(upd);
          if (res->path != NULL)
            sqlite3_bind_text(upd, 1, res->path, -1, SQLITE_TRANSIENT);
          else
            sqlite3_bind_null(upd, 1);
          sqlite3_bind_text(upd, 2, bt_link_status_name(res->status), -1, SQLITE_STATIC);
          sqlite3_bind_text(upd, 3, bt_link_kind_name(res->kind), -1, SQLITE_STATIC);
          sqlite3_bind_int64(upd, 4, r->rowid);
          sqlite3_step(upd);
        }
        bt_link_resolution_free(res);
      }
      g_free(r->src_path);
      g_free(r->raw_target);
    }
    if (upd != NULL) sqlite3_finalize(upd);
    if (del != NULL) sqlite3_finalize(del);
    g_ptr_array_unref(rows);
    bt_index_snapshot_free(snap);
  }
  c->ok = exec_sql(db, "COMMIT;", c->error);
}

gboolean bt_index_upsert_after_create(BtIndex *idx, const BtVault *vault, const gchar *relpath,
                                      GError **error)
{
  g_return_val_if_fail(idx != NULL && vault != NULL && relpath != NULL, FALSE);
  gchar *raw = NULL;
  gsize len = 0;
  if (!vault_read(vault, relpath, &raw, &len, error))
    return FALSE;
  UpsertCtx c = {
    .vault = vault, .relpath = relpath, .raw = raw, .raw_len = len,
    .skipped = NULL, .error = error, .ok = FALSE,
  };
  run_on_writer(idx, job_upsert_created, &c);
  g_free(raw);
  return c.ok;
}

typedef struct {
  const gchar *relpath;
  GError **error;
  gboolean ok;
} DelCtx;

static void job_delete_path(sqlite3 *db, gpointer ud)
{
  DelCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "DELETE FROM notes WHERE path=?1;", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->relpath, -1, SQLITE_TRANSIENT);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
  if (c->ok)
    re_resolve_all_in_db(db, NULL);
}

gboolean bt_index_delete_path(BtIndex *idx, const gchar *relpath, GError **error)
{
  DelCtx c = {.relpath = relpath, .error = error, .ok = FALSE};
  run_on_writer(idx, job_delete_path, &c);
  return c.ok;
}

typedef struct {
  const BtVault *vault;
  GError **error;
  gboolean ok;
} RebuildCtx;

static void job_rebuild(sqlite3 *db, gpointer ud)
{
  RebuildCtx *c = ud;
  if (!exec_sql(db, "BEGIN IMMEDIATE; DELETE FROM link_resolutions; DELETE FROM edges; DELETE FROM tags; DELETE FROM notes;",
                c->error))
    return;

  const GPtrArray *notes = vault_notes(c->vault);
  for (guint i = 0; i < notes->len; i++) {
    const gchar *rel = g_ptr_array_index(notes, i);
    gchar *raw = NULL;
    gsize len = 0;
    if (!vault_read(c->vault, rel, &raw, &len, NULL))
      continue;
    gboolean ok = upsert_in_db(db, rel, raw, len, NULL, c->error);
    g_free(raw);
    if (!ok) {
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      return;
    }
  }
  re_resolve_all_in_db(db, c->vault);
  c->ok = exec_sql(db, "COMMIT;", c->error);
}

gboolean bt_index_rebuild(BtIndex *idx, const BtVault *vault, GError **error)
{
  g_return_val_if_fail(idx != NULL && vault != NULL, FALSE);
  RebuildCtx c = {.vault = vault, .error = error, .ok = FALSE};
  run_on_writer(idx, job_rebuild, &c);
  return c.ok;
}

/* ----------------------------------------------------- queries & search -- */

typedef struct {
  const gchar *query;
  guint limit;
  GPtrArray *out;
  GError **error;
} SearchCtx;

static gchar *sanitize_fts_query(const gchar *q)
{
  if (q == NULL)
    return g_strdup("");
  /* Tokenize on whitespace and quote each term with prefix '*' for instant search. */
  gchar **toks = g_strsplit_set(q, " \t\r\n\"'():^*-", -1);
  GString *s = g_string_new(NULL);
  for (guint i = 0; toks[i] != NULL; i++) {
    if (toks[i][0] == '\0')
      continue;
    if (s->len > 0)
      g_string_append_c(s, ' ');
    g_string_append_printf(s, "\"%s\"*", toks[i]);
  }
  g_strfreev(toks);
  return g_string_free(s, FALSE);
}

static void job_search_fts(sqlite3 *db, gpointer ud)
{
  SearchCtx *c = ud;
  c->out = g_ptr_array_new_with_free_func(bt_search_hit_free);
  gchar *fq = sanitize_fts_query(c->query);
  if (fq[0] == '\0') {
    g_free(fq);
    return;
  }
  const char *sql =
      "SELECT n.id, n.path, n.title, snippet(notes_fts, 1, '[', ']', '...', 16), bm25(notes_fts) "
      "FROM notes_fts JOIN notes n ON n.rowid = notes_fts.rowid "
      "WHERE notes_fts MATCH ?1 ORDER BY bm25(notes_fts), n.path LIMIT ?2;";
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, fq, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, (int)(c->limit > 0 ? c->limit : 50));
    while (sqlite3_step(st) == SQLITE_ROW) {
      BtSearchHit *h = g_new0(BtSearchHit, 1);
      h->note_id = g_strdup((const char *)sqlite3_column_text(st, 0));
      h->path = g_strdup((const char *)sqlite3_column_text(st, 1));
      h->title = g_strdup((const char *)sqlite3_column_text(st, 2));
      const char *sn = (const char *)sqlite3_column_text(st, 3);
      h->snippet = g_strdup(sn ? sn : "");
      h->rank = sqlite3_column_double(st, 4);
      g_ptr_array_add(c->out, h);
    }
    sqlite3_finalize(st);
  }
  g_free(fq);
}

GPtrArray *bt_index_search_fts(BtIndex *idx, const gchar *query, guint limit, GError **error)
{
  g_return_val_if_fail(idx != NULL, NULL);
  SearchCtx c = {.query = query, .limit = limit, .out = NULL, .error = error};
  run_on_writer(idx, job_search_fts, &c);
  return c.out;
}

typedef struct {
  int val;
  const char *sql;
  const char *arg;
} IntQueryCtx;

static void job_int_query(sqlite3 *db, gpointer ud)
{
  IntQueryCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, c->sql, -1, &st, NULL) == SQLITE_OK) {
    if (c->arg != NULL)
      sqlite3_bind_text(st, 1, c->arg, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
      c->val = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
}

gint bt_index_user_version(BtIndex *idx)
{
  IntQueryCtx c = {.val = 0, .sql = "PRAGMA user_version;", .arg = NULL};
  run_on_writer(idx, job_int_query, &c);
  return c.val;
}

guint bt_index_note_count(BtIndex *idx)
{
  IntQueryCtx c = {.val = 0, .sql = "SELECT COUNT(*) FROM notes;", .arg = NULL};
  run_on_writer(idx, job_int_query, &c);
  return (guint)MAX(c.val, 0);
}

gint bt_index_count_fts_matches(BtIndex *idx, const gchar *query)
{
  gchar *fq = sanitize_fts_query(query);
  IntQueryCtx c = {.val = 0, .sql = "SELECT COUNT(*) FROM notes_fts WHERE notes_fts MATCH ?1;",
                   .arg = fq};
  run_on_writer(idx, job_int_query, &c);
  g_free(fq);
  return c.val;
}

typedef struct {
  gchar *dump;
} DumpCtx;

static void job_dump(sqlite3 *db, gpointer ud)
{
  DumpCtx *c = ud;
  GString *s = g_string_new(NULL);
  sqlite3_stmt *st = NULL;

  if (sqlite3_prepare_v2(
          db,
          "SELECT id, path, title, created, updated, source, COALESCE(agent,''), fm_hash, body_hash, parse_error "
          "FROM notes ORDER BY path;",
          -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      g_string_append_printf(s, "N|%s|%s|%s|%lld|%lld|%s|%s|%s|%s|%d\n",
                             sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                             sqlite3_column_text(st, 2), (long long)sqlite3_column_int64(st, 3),
                             (long long)sqlite3_column_int64(st, 4), sqlite3_column_text(st, 5),
                             sqlite3_column_text(st, 6), sqlite3_column_text(st, 7),
                             sqlite3_column_text(st, 8), sqlite3_column_int(st, 9));
    }
    sqlite3_finalize(st);
  }
  if (sqlite3_prepare_v2(db, "SELECT note_id, tag, origin, COALESCE(confidence,-1) FROM tags ORDER BY note_id, tag, origin;",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      g_string_append_printf(s, "T|%s|%s|%s|%.3f\n", sqlite3_column_text(st, 0),
                             sqlite3_column_text(st, 1), sqlite3_column_text(st, 2),
                             sqlite3_column_double(st, 3));
    }
    sqlite3_finalize(st);
  }
  if (sqlite3_prepare_v2(db, "SELECT src, dst, kind, weight FROM edges ORDER BY src, dst, kind;",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      g_string_append_printf(s, "E|%s|%s|%s|%.3f\n", sqlite3_column_text(st, 0),
                             sqlite3_column_text(st, 1), sqlite3_column_text(st, 2),
                             sqlite3_column_double(st, 3));
    }
    sqlite3_finalize(st);
  }
  c->dump = g_string_free(s, FALSE);
}

gchar *bt_index_dump_canonical(BtIndex *idx, GError **error)
{
  (void)error;
  DumpCtx c = {NULL};
  run_on_writer(idx, job_dump, &c);
  return c.dump;
}

/* ------------------------------------------- sessions & extra queries -- */

typedef struct {
  const gchar *id, *tool, *cwd, *resume_cmd, *note_id;
  gint64 started, ended;
  gboolean ok;
} SessCtx;

static void job_sess_upsert(sqlite3 *db, gpointer ud)
{
  SessCtx *c = ud;
  sqlite3_stmt *st = NULL;
  const char *sql =
      "INSERT OR REPLACE INTO sessions(id, tool, cwd, started, ended, resume_cmd, note_id) "
      "VALUES(?1,?2,?3,?4,?5,?6,(SELECT id FROM notes WHERE id=?7 OR path=?7 LIMIT 1));";
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c->tool, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c->cwd, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, c->started);
    if (c->ended > 0)
      sqlite3_bind_int64(st, 5, c->ended);
    else
      sqlite3_bind_null(st, 5);
    sqlite3_bind_text(st, 6, c->resume_cmd ? c->resume_cmd : "", -1, SQLITE_TRANSIENT);
    if (c->note_id)
      sqlite3_bind_text(st, 7, c->note_id, -1, SQLITE_TRANSIENT);
    else
      sqlite3_bind_null(st, 7);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
}

gboolean bt_index_session_upsert(BtIndex *idx, const gchar *id, const gchar *tool,
                                 const gchar *cwd, gint64 started, gint64 ended,
                                 const gchar *resume_cmd, const gchar *note_id, GError **error)
{
  (void)error;
  SessCtx c = {.id = id, .tool = tool, .cwd = cwd, .started = started, .ended = ended,
               .resume_cmd = resume_cmd, .note_id = note_id, .ok = FALSE};
  run_on_writer(idx, job_sess_upsert, &c);
  return c.ok;
}

static void job_sess_end(sqlite3 *db, gpointer ud)
{
  SessCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "UPDATE sessions SET ended=?1 WHERE id=?2;", -1, &st, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_int64(st, 1, c->ended);
    sqlite3_bind_text(st, 2, c->id, -1, SQLITE_TRANSIENT);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
}

gboolean bt_index_session_end(BtIndex *idx, const gchar *id, gint64 ended, GError **error)
{
  (void)error;
  SessCtx c = {.id = id, .ended = ended, .ok = FALSE};
  run_on_writer(idx, job_sess_end, &c);
  return c.ok;
}

gboolean bt_index_import_sessions_json(BtIndex *idx, const gchar *vault_root, GError **error)
{
  gchar *jp = bt_sessions_json_path(vault_root);
  if (!g_file_test(jp, G_FILE_TEST_EXISTS)) {
    g_free(jp);
    return TRUE;
  }
  g_free(jp);
  BtSessionStore *st = bt_sessions_open(vault_root, error);
  if (st == NULL)
    return FALSE;
  const GPtrArray *items = bt_sessions_list(st);
  for (guint i = 0; i < items->len; i++) {
    const BtSession *s = g_ptr_array_index(items, i);
    bt_index_session_upsert(idx, s->id, s->tool, s->cwd, s->started, s->ended, s->resume_cmd,
                            s->note_id, NULL);
  }
  bt_sessions_free(st);
  return TRUE;
}

typedef struct {
  const gchar *arg;
  GPtrArray *out;
  gchar *str_out;
} ListCtx;

static void job_list_notes(sqlite3 *db, gpointer ud)
{
  ListCtx *c = ud;
  c->out = g_ptr_array_new_with_free_func(bt_note_row_free);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db,
                         "SELECT id, path, title, created, updated, source, agent, fm_hash, body_hash, parse_error "
                         "FROM notes ORDER BY path;",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      BtNoteRow *r = g_new0(BtNoteRow, 1);
      r->id = g_strdup((const char *)sqlite3_column_text(st, 0));
      r->path = g_strdup((const char *)sqlite3_column_text(st, 1));
      r->title = g_strdup((const char *)sqlite3_column_text(st, 2));
      r->created = sqlite3_column_int64(st, 3);
      r->updated = sqlite3_column_int64(st, 4);
      r->source = g_strdup((const char *)sqlite3_column_text(st, 5));
      const char *ag = (const char *)sqlite3_column_text(st, 6);
      r->agent = ag ? g_strdup(ag) : NULL;
      r->fm_hash = g_strdup((const char *)sqlite3_column_text(st, 7));
      r->body_hash = g_strdup((const char *)sqlite3_column_text(st, 8));
      r->parse_error = sqlite3_column_int(st, 9) != 0;
      g_ptr_array_add(c->out, r);
    }
    sqlite3_finalize(st);
  }
}

GPtrArray *bt_index_list_notes(BtIndex *idx, GError **error)
{
  (void)error;
  ListCtx c = {0};
  run_on_writer(idx, job_list_notes, &c);
  return c.out;
}

static void job_list_tags(sqlite3 *db, gpointer ud)
{
  ListCtx *c = ud;
  c->out = g_ptr_array_new_with_free_func(bt_tag_row_free);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT tag, origin, confidence FROM tags WHERE note_id=?1 ORDER BY origin DESC, tag;",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->arg, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
      BtTagRow *r = g_new0(BtTagRow, 1);
      r->tag = g_strdup((const char *)sqlite3_column_text(st, 0));
      r->origin = g_strdup((const char *)sqlite3_column_text(st, 1));
      r->has_confidence = sqlite3_column_type(st, 2) != SQLITE_NULL;
      r->confidence = r->has_confidence ? sqlite3_column_double(st, 2) : 0.0;
      g_ptr_array_add(c->out, r);
    }
    sqlite3_finalize(st);
  }
}

GPtrArray *bt_index_list_tags(BtIndex *idx, const gchar *note_id, GError **error)
{
  (void)error;
  ListCtx c = {.arg = note_id};
  run_on_writer(idx, job_list_tags, &c);
  return c.out;
}

static void job_list_edges(sqlite3 *db, gpointer ud)
{
  ListCtx *c = ud;
  c->out = g_ptr_array_new_with_free_func(bt_edge_row_free);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT src, dst, kind, weight FROM edges ORDER BY src, dst, kind;",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      BtEdgeRow *r = g_new0(BtEdgeRow, 1);
      r->src = g_strdup((const char *)sqlite3_column_text(st, 0));
      r->dst = g_strdup((const char *)sqlite3_column_text(st, 1));
      r->kind = g_strdup((const char *)sqlite3_column_text(st, 2));
      r->weight = sqlite3_column_double(st, 3);
      g_ptr_array_add(c->out, r);
    }
    sqlite3_finalize(st);
  }
}

GPtrArray *bt_index_list_edges(BtIndex *idx, GError **error)
{
  (void)error;
  ListCtx c = {0};
  run_on_writer(idx, job_list_edges, &c);
  return c.out;
}

static void job_list_link_resolutions(sqlite3 *db, gpointer ud)
{
  ListCtx *c = ud;
  c->out = g_ptr_array_new_with_free_func(bt_link_res_row_free);
  sqlite3_stmt *st = NULL;
  const char *sql = c->arg
      ? "SELECT src_note_id, src_path, raw_target, resolved_path, status, kind, line_no "
        "FROM link_resolutions WHERE src_note_id=?1 ORDER BY line_no, id;"
      : "SELECT src_note_id, src_path, raw_target, resolved_path, status, kind, line_no "
        "FROM link_resolutions ORDER BY src_path, line_no, id;";
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
    if (c->arg != NULL)
      sqlite3_bind_text(st, 1, c->arg, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
      BtLinkResRow *r = g_new0(BtLinkResRow, 1);
      r->src_note_id = g_strdup((const char *)sqlite3_column_text(st, 0));
      r->src_path = g_strdup((const char *)sqlite3_column_text(st, 1));
      r->raw_target = g_strdup((const char *)sqlite3_column_text(st, 2));
      const char *rp = (const char *)sqlite3_column_text(st, 3);
      r->resolved_path = rp ? g_strdup(rp) : NULL;
      r->status = g_strdup((const char *)sqlite3_column_text(st, 4));
      r->kind = g_strdup((const char *)sqlite3_column_text(st, 5));
      r->line_no = sqlite3_column_int(st, 6);
      g_ptr_array_add(c->out, r);
    }
    sqlite3_finalize(st);
  }
}

GPtrArray *bt_index_list_link_resolutions(BtIndex *idx, const gchar *src_note_id, GError **error)
{
  (void)error;
  ListCtx c = {.arg = src_note_id};
  run_on_writer(idx, job_list_link_resolutions, &c);
  return c.out;
}

static void job_body_hash(sqlite3 *db, gpointer ud)
{
  ListCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT body_hash FROM notes WHERE path=?1;", -1, &st, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->arg, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
      c->str_out = g_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
}

gchar *bt_index_get_body_hash(BtIndex *idx, const gchar *relpath)
{
  ListCtx c = {.arg = relpath};
  run_on_writer(idx, job_body_hash, &c);
  return c.str_out;
}

typedef struct {
  const gchar *note_id, *tag, *origin;
  gdouble conf;
  gboolean has_conf;
  gboolean ok;
} TagOpCtx;

static void job_set_tag(sqlite3 *db, gpointer ud)
{
  TagOpCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO tags(note_id, tag, origin, confidence) VALUES(?1,?2,?3,?4);",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c->tag, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c->origin, -1, SQLITE_TRANSIENT);
    if (c->has_conf)
      sqlite3_bind_double(st, 4, c->conf);
    else
      sqlite3_bind_null(st, 4);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
}

gboolean bt_index_set_tag(BtIndex *idx, const gchar *note_id, const gchar *tag,
                          const gchar *origin, gdouble confidence, gboolean has_conf,
                          GError **error)
{
  (void)error;
  TagOpCtx c = {.note_id = note_id, .tag = tag, .origin = origin, .conf = confidence,
                .has_conf = has_conf};
  run_on_writer(idx, job_set_tag, &c);
  return c.ok;
}

static void job_del_tag(sqlite3 *db, gpointer ud)
{
  TagOpCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "DELETE FROM tags WHERE note_id=?1 AND tag=?2 AND origin=?3;", -1,
                         &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c->tag, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c->origin, -1, SQLITE_TRANSIENT);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
}

gboolean bt_index_delete_tag(BtIndex *idx, const gchar *note_id, const gchar *tag,
                             const gchar *origin, GError **error)
{
  (void)error;
  TagOpCtx c = {.note_id = note_id, .tag = tag, .origin = origin};
  run_on_writer(idx, job_del_tag, &c);
  return c.ok;
}

typedef struct {
  const gchar *note_id;
  gint chunk;
  const gfloat *vec;
  gsize dim;
  gboolean ok;
} EmbCtx;

static void job_put_emb(sqlite3 *db, gpointer ud)
{
  EmbCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO embeddings(note_id, chunk, vec) VALUES(?1,?2,?3);",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->note_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, c->chunk);
    sqlite3_bind_blob(st, 3, c->vec, (int)(c->dim * sizeof(gfloat)), SQLITE_TRANSIENT);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
}

gboolean bt_index_put_embedding(BtIndex *idx, const gchar *note_id, gint chunk,
                                const gfloat *vec, gsize dim, GError **error)
{
  (void)error;
  EmbCtx c = {.note_id = note_id, .chunk = chunk, .vec = vec, .dim = dim};
  run_on_writer(idx, job_put_emb, &c);
  return c.ok;
}

static void job_clear_emb(sqlite3 *db, gpointer ud)
{
  EmbCtx *c = ud;
  c->ok = sqlite3_exec(db, "DELETE FROM embeddings; DELETE FROM edges WHERE kind='similar';", NULL,
                       NULL, NULL) == SQLITE_OK;
}

gboolean bt_index_clear_embeddings(BtIndex *idx, GError **error)
{
  (void)error;
  EmbCtx c = {0};
  run_on_writer(idx, job_clear_emb, &c);
  return c.ok;
}

typedef struct {
  const gchar *src, *dst, *kind;
  gdouble weight;
  gboolean ok;
} EdgeOpCtx;

static void job_put_edge(sqlite3 *db, gpointer ud)
{
  EdgeOpCtx *c = ud;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO edges(src, dst, kind, weight) VALUES(?1,?2,?3,?4);",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, c->src, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c->dst, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c->kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 4, c->weight);
    c->ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
  }
}

gboolean bt_index_put_edge(BtIndex *idx, const gchar *src, const gchar *dst, const gchar *kind,
                           gdouble weight, GError **error)
{
  (void)error;
  EdgeOpCtx c = {.src = src, .dst = dst, .kind = kind, .weight = weight};
  run_on_writer(idx, job_put_edge, &c);
  return c.ok;
}
