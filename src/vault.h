/* vault.h — vault open/scan, path guard, atomic write (§6.A1, §6.A3, §6.A4).
 *
 * All paths handed to this module are vault-relative with '/' separators.
 * vault_resolve() is the single choke point turning them into filesystem
 * paths; every read and write in BrainTerm goes through it. */
#ifndef BT_VAULT_H
#define BT_VAULT_H

#include <glib.h>

#include "bterror.h"

G_BEGIN_DECLS

typedef struct BtVault BtVault;

typedef struct {
  guint notes;            /* .md files found */
  guint tmp_removed;      /* *.md.tmp residue deleted (R1) */
  guint dirs_unreadable;  /* subdirectories skipped due to errors */
  gint64 elapsed_us;      /* wall time of the scan */
} BtScanStats;

/* Stages of vault_atomic_write(), exposed for the crash rig (§11.3). */
typedef enum {
  BT_WRITE_STAGE_TMP_OPENED,
  BT_WRITE_STAGE_TMP_PARTIAL,
  BT_WRITE_STAGE_TMP_SYNCED,
  BT_WRITE_STAGE_BEFORE_RENAME,
  BT_WRITE_STAGE_AFTER_RENAME,
  BT_WRITE_STAGE_COUNT
} BtWriteStage;

typedef struct {
  void (*at_stage)(BtWriteStage stage, void *user_data);
  void *user_data;
} BtWriteHooks;

/* Opens a directory as a vault: canonicalizes the root, removes atomic-write
 * residue and performs the first scan. stats may be NULL. */
BtVault *vault_open(const gchar *path, BtScanStats *stats, GError **error);
void vault_free(BtVault *vault);

/* Canonical (link-resolved) absolute root. */
const gchar *vault_root(const BtVault *vault);

/* Sorted vault-relative paths of every note found by the last scan. */
const GPtrArray *vault_notes(const BtVault *vault);

gboolean vault_rescan(BtVault *vault, BtScanStats *stats, GError **error);

/* Adds a user ignore pattern (glob, matched against basename and relpath). */
void vault_add_ignore_pattern(BtVault *vault, const gchar *glob);

/* TRUE if any component of relpath is ignored (§7.4). */
gboolean vault_is_ignored_path(const BtVault *vault, const gchar *relpath);

/* The path guard (§6.A4, §8.3). Returns a newly allocated absolute path
 * strictly inside the vault root, or NULL with BT_ERROR_PATH_REJECTED. */
gchar *vault_resolve(const BtVault *vault, const gchar *relpath, GError **error);

gboolean vault_read(const BtVault *vault, const gchar *relpath, gchar **contents, gsize *length,
                    GError **error);

/* Atomic write (§6.A3): tmp (exclusive create) -> fsync -> rename -> dir
 * sync. Missing parent directories inside the vault are created. hooks may
 * be NULL; it exists only for the crash rig. */
gboolean vault_atomic_write(const BtVault *vault, const gchar *relpath, const gchar *data,
                            gsize length, const BtWriteHooks *hooks, GError **error);

G_END_DECLS

#endif
