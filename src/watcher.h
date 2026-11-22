/* watcher.h — Vault GFileMonitor with 500 ms debounce, storm guard (R4),
 * and R3 differential body_hash audit (§6.A5, §7.3, §9 R3/R4). */
#ifndef BT_WATCHER_H
#define BT_WATCHER_H

#include <glib.h>

#include "index.h"
#include "vault.h"

G_BEGIN_DECLS

#define BT_WATCHER_DEBOUNCE_MS     500u
#define BT_WATCHER_STORM_THRESHOLD 500u

typedef enum {
  BT_WATCH_CHANGED,
  BT_WATCH_DELETED,
  BT_WATCH_STORM_RESCAN
} BtWatchEventKind;

typedef enum {
  BT_CONFLICT_NOOP,          /* same content hash as buffer */
  BT_CONFLICT_SILENT_RELOAD, /* buffer clean, disk changed */
  BT_CONFLICT_BANNER_DIRTY,  /* buffer dirty, disk changed -> show banner */
  BT_CONFLICT_BANNER_DELETED /* file deleted on disk -> show banner */
} BtConflictAction;

typedef struct BtWatcher BtWatcher;

typedef void (*BtWatcherEventFn)(BtWatchEventKind kind, const gchar *relpath, gpointer user_data);

BtWatcher *bt_watcher_new(const BtVault *vault, guint debounce_ms, BtWatcherEventFn on_event,
                          gpointer user_data);
void bt_watcher_free(BtWatcher *w);

/* Feeds a synthetic event (used by tests to exercise storm threshold R4). */
void bt_watcher_inject_event(BtWatcher *w, const gchar *relpath, gboolean deleted);

/* Pure conflict decision function (§6.A5). */
BtConflictAction bt_conflict_decide(gboolean buffer_dirty, gboolean file_deleted,
                                    const gchar *buffer_hash, const gchar *disk_hash);

/* R3 differential audit: compares all vault .md files vs SQLite body_hash,
 * re-indexes any drifted notes, and returns the count of repaired notes. */
guint bt_watcher_audit_drift(const BtVault *vault, BtIndex *index, GPtrArray **drifted_paths_out);

G_END_DECLS

#endif
