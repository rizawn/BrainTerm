/* sessions.h — Session registry persisted to vault/.bt/sessions.json (M3)
 * and imported into SQLite sessions table in M4 (§6.D6, §7.2). */
#ifndef BT_SESSIONS_H
#define BT_SESSIONS_H

#include <glib.h>

#include "bterror.h"
#include "checkpoint.h"

G_BEGIN_DECLS

typedef struct {
  gchar *id;
  gchar *tool; /* shell | claude-code | codex | gemini */
  gchar *cwd;
  gint64 started; /* unix ms */
  gint64 ended;   /* unix ms, 0 if running/aborted */
  gchar *resume_cmd;
  gchar *note_id;
  gchar checkpoint_oid[BT_OID_HEX_LEN];
} BtSession;

typedef struct BtSessionStore BtSessionStore;

void bt_session_free(gpointer session);

/* Loads existing vault/.bt/sessions.json if present (never opens SQLite in M3). */
BtSessionStore *bt_sessions_open(const gchar *vault_root, GError **error);
void bt_sessions_free(BtSessionStore *store);

/* Records a new session and persists sessions.json. Returns borrowed pointer. */
const BtSession *bt_sessions_add(BtSessionStore *store, const gchar *tool, const gchar *cwd,
                                 const gchar *resume_cmd, const gchar *note_id,
                                 const gchar *checkpoint_oid, GError **error);

gboolean bt_sessions_mark_ended(BtSessionStore *store, const gchar *session_id, GError **error);
const BtSession *bt_sessions_find(const BtSessionStore *store, const gchar *session_id);
const GPtrArray *bt_sessions_list(const BtSessionStore *store);
gchar *bt_sessions_json_path(const gchar *vault_root);

G_END_DECLS

#endif
