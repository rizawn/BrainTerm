/* checkpoint.h — libgit2 in-process checkpoint & rollback façade (ADR-006, P7, R10). */
#ifndef BT_CHECKPOINT_H
#define BT_CHECKPOINT_H

#include <glib.h>

#include "bterror.h"

G_BEGIN_DECLS

#define BT_OID_HEX_LEN 41 /* 40 hex chars + NUL */

typedef struct {
  gchar oid[BT_OID_HEX_LEN];
  gchar *message;
  gint64 timestamp; /* unix seconds */
} BtCheckpointInfo;

void bt_checkpoint_info_free(gpointer info);

/* Process-wide libgit2 init/shutdown (refcounted by libgit2). */
void bt_checkpoint_init(void);
void bt_checkpoint_shutdown(void);

/* Stages all vault changes and commits with identity
 * "brainterm-agent <noreply@brainterm.local>". Initializes .git transparently
 * if absent. On error (e.g. locked .git), returns FALSE and sets error (R10). */
gboolean bt_checkpoint_take(const gchar *vault_root, const gchar *message,
                            gchar oid_out[BT_OID_HEX_LEN], GError **error);

/* Hard-resets the vault repository to ref_or_oid (P7 rollback). */
gboolean bt_checkpoint_rollback(const gchar *vault_root, const gchar *ref_or_oid, GError **error);

/* Lists up to max_count recent commits (GPtrArray of BtCheckpointInfo*). */
GPtrArray *bt_checkpoint_list(const gchar *vault_root, guint max_count, GError **error);

/* Reads a file's contents as it existed at ref_or_oid (for M5 Review diff). */
gboolean bt_checkpoint_read_file(const gchar *vault_root, const gchar *ref_or_oid,
                                 const gchar *relpath, gchar **contents, gsize *length,
                                 GError **error);

G_END_DECLS

#endif
