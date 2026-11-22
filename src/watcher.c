/* watcher.c — GFileMonitor watcher with 500 ms debounce + R4 storm guard + R3 audit. */
#include "watcher.h"
#include "frontmatter.h"

#include <gio/gio.h>
#include <string.h>

struct BtWatcher {
  const BtVault *vault;
  guint debounce_ms;
  BtWatcherEventFn on_event;
  gpointer user_data;

  GPtrArray *monitors;   /* GFileMonitor* */
  GHashTable *pending;   /* relpath -> GINT_TO_POINTER(BtWatchEventKind) */
  guint raw_event_count;
  gboolean storm_triggered;
  guint timer_id;
};

static gboolean flush_pending(gpointer ud)
{
  BtWatcher *w = ud;
  w->timer_id = 0;

  if (w->storm_triggered) {
    w->storm_triggered = FALSE;
    w->raw_event_count = 0;
    g_hash_table_remove_all(w->pending);
    if (w->on_event != NULL)
      w->on_event(BT_WATCH_STORM_RESCAN, NULL, w->user_data);
    return G_SOURCE_REMOVE;
  }

  GHashTableIter it;
  gpointer k, v;
  GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
  GArray *kinds = g_array_new(FALSE, FALSE, sizeof(BtWatchEventKind));
  g_hash_table_iter_init(&it, w->pending);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    g_ptr_array_add(keys, g_strdup(k));
    BtWatchEventKind ek = (BtWatchEventKind)GPOINTER_TO_INT(v);
    g_array_append_val(kinds, ek);
  }
  g_hash_table_remove_all(w->pending);
  w->raw_event_count = 0;

  for (guint i = 0; i < keys->len; i++) {
    if (w->on_event != NULL)
      w->on_event(g_array_index(kinds, BtWatchEventKind, i), g_ptr_array_index(keys, i),
                  w->user_data);
  }
  g_ptr_array_unref(keys);
  g_array_unref(kinds);
  return G_SOURCE_REMOVE;
}

static void enqueue_event(BtWatcher *w, const gchar *relpath, gboolean deleted)
{
  if (relpath == NULL || relpath[0] == '\0')
    return;
  if (vault_is_ignored_path(w->vault, relpath))
    return;
  if (!g_str_has_suffix(relpath, ".md") && !g_str_has_suffix(relpath, ".MD"))
    return;

  w->raw_event_count++;
  if (w->raw_event_count > BT_WATCHER_STORM_THRESHOLD)
    w->storm_triggered = TRUE;
  else
    g_hash_table_insert(w->pending, g_strdup(relpath),
                        GINT_TO_POINTER(deleted ? BT_WATCH_DELETED : BT_WATCH_CHANGED));

  if (w->timer_id != 0)
    g_source_remove(w->timer_id);
  w->timer_id = g_timeout_add(w->debounce_ms, flush_pending, w);
}

static gchar *abs_to_rel(const BtVault *vault, const gchar *abs_path)
{
  const gchar *root = vault_root(vault);
  gsize rl = strlen(root);
  if (abs_path == NULL || strlen(abs_path) <= rl)
    return NULL;
#ifdef G_OS_WIN32
  if (g_ascii_strncasecmp(abs_path, root, rl) != 0)
    return NULL;
#else
  if (strncmp(abs_path, root, rl) != 0)
    return NULL;
#endif
  const gchar *p = abs_path + rl;
  while (G_IS_DIR_SEPARATOR(*p))
    p++;
  if (*p == '\0')
    return NULL;
  gchar *rel = g_strdup(p);
  for (gchar *c = rel; *c != '\0'; c++)
    if (*c == '\\')
      *c = '/';
  return rel;
}

static void on_fs_changed(GFileMonitor *mon, GFile *file, GFile *other, GFileMonitorEvent ev,
                          gpointer ud)
{
  (void)mon;
  BtWatcher *w = ud;
  gchar *path = file ? g_file_get_path(file) : NULL;
  gchar *rel = abs_to_rel(w->vault, path);
  g_free(path);

  if (rel != NULL) {
    gboolean del = (ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_MOVED_OUT);
    if (ev == G_FILE_MONITOR_EVENT_CREATED || ev == G_FILE_MONITOR_EVENT_CHANGED ||
        ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_MOVED_IN ||
        ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_MOVED_OUT) {
      enqueue_event(w, rel, del);
    }
    g_free(rel);
  }
  if (other != NULL && ev == G_FILE_MONITOR_EVENT_RENAMED) {
    gchar *opath = g_file_get_path(other);
    gchar *orel = abs_to_rel(w->vault, opath);
    g_free(opath);
    if (orel != NULL) {
      enqueue_event(w, orel, FALSE);
      g_free(orel);
    }
  }
}

static void watch_dir_recursive(BtWatcher *w, const gchar *abs_dir, guint depth)
{
  if (depth > 32)
    return;
  GFile *gf = g_file_new_for_path(abs_dir);
  GFileMonitor *mon = g_file_monitor_directory(gf, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  g_object_unref(gf);
  if (mon != NULL) {
    g_signal_connect(mon, "changed", G_CALLBACK(on_fs_changed), w);
    g_ptr_array_add(w->monitors, mon);
  }

  GDir *d = g_dir_open(abs_dir, 0, NULL);
  if (d == NULL)
    return;
  const gchar *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    gchar *child = g_build_filename(abs_dir, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
      gchar *rel = abs_to_rel(w->vault, child);
      if (rel != NULL && !vault_is_ignored_path(w->vault, rel))
        watch_dir_recursive(w, child, depth + 1);
      g_free(rel);
    }
    g_free(child);
  }
  g_dir_close(d);
}

BtWatcher *bt_watcher_new(const BtVault *vault, guint debounce_ms, BtWatcherEventFn on_event,
                          gpointer user_data)
{
  g_return_val_if_fail(vault != NULL, NULL);
  BtWatcher *w = g_new0(BtWatcher, 1);
  w->vault = vault;
  w->debounce_ms = debounce_ms > 0 ? debounce_ms : BT_WATCHER_DEBOUNCE_MS;
  w->on_event = on_event;
  w->user_data = user_data;
  w->monitors = g_ptr_array_new_with_free_func(g_object_unref);
  w->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  watch_dir_recursive(w, vault_root(vault), 0);
  return w;
}

void bt_watcher_free(BtWatcher *w)
{
  if (w == NULL)
    return;
  if (w->timer_id != 0)
    g_source_remove(w->timer_id);
  for (guint i = 0; i < w->monitors->len; i++)
    g_file_monitor_cancel(g_ptr_array_index(w->monitors, i));
  g_ptr_array_unref(w->monitors);
  g_hash_table_destroy(w->pending);
  g_free(w);
}

void bt_watcher_inject_event(BtWatcher *w, const gchar *relpath, gboolean deleted)
{
  g_return_if_fail(w != NULL);
  enqueue_event(w, relpath, deleted);
}

BtConflictAction bt_conflict_decide(gboolean buffer_dirty, gboolean file_deleted,
                                    const gchar *buffer_hash, const gchar *disk_hash)
{
  if (file_deleted)
    return BT_CONFLICT_BANNER_DELETED;
  if (buffer_hash != NULL && disk_hash != NULL && strcmp(buffer_hash, disk_hash) == 0)
    return BT_CONFLICT_NOOP;
  return buffer_dirty ? BT_CONFLICT_BANNER_DIRTY : BT_CONFLICT_SILENT_RELOAD;
}

guint bt_watcher_audit_drift(const BtVault *vault, BtIndex *index, GPtrArray **drifted_paths_out)
{
  g_return_val_if_fail(vault != NULL && index != NULL, 0);
  GPtrArray *drifted = g_ptr_array_new_with_free_func(g_free);
  const GPtrArray *notes = vault_notes(vault);

  for (guint i = 0; i < notes->len; i++) {
    const gchar *rel = g_ptr_array_index(notes, i);
    gchar *raw = NULL;
    gsize len = 0;
    if (!vault_read(vault, rel, &raw, &len, NULL))
      continue;
    BtFrontmatter *fm = bt_frontmatter_parse(raw, len);
    const gchar *body = raw + fm->body_offset;
    gsize blen = (len >= fm->body_offset) ? (len - fm->body_offset) : 0;
    gchar *disk_hash = bt_fnv1a_hex(body, blen);
    gchar *db_hash = bt_index_get_body_hash(index, rel);

    if (db_hash == NULL || strcmp(db_hash, disk_hash) != 0) {
      g_ptr_array_add(drifted, g_strdup(rel));
      bt_index_upsert_note(index, vault, rel, NULL, NULL);
    }
    g_free(db_hash);
    g_free(disk_hash);
    bt_frontmatter_free(fm);
    g_free(raw);
  }

  guint count = drifted->len;
  if (drifted_paths_out != NULL)
    *drifted_paths_out = drifted;
  else
    g_ptr_array_unref(drifted);
  return count;
}
