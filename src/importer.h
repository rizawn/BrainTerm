/* importer.h — Chat & agent session importers (ChatGPT, Claude, Claude Code,
 * Codex, Gemini) with P3 separation, original dates, idempotency, and
 * vault_resolve() path guard (§6.B1, §7.4, §8.3). */
#ifndef BT_IMPORTER_H
#define BT_IMPORTER_H

#include <glib.h>

#include "bterror.h"
#include "index.h"
#include "vault.h"

G_BEGIN_DECLS

typedef struct {
  guint imported;
  guint skipped;  /* already existed (idempotent) */
  guint rejected; /* hostile path or malformed entry */
} BtImportStats;

/* ChatGPT / Claude web conversations.json -> inbox/import/<tool>/YYYY-MM-DD-<slug>.md */
gboolean bt_import_conversations_json(const BtVault *vault, BtIndex *index,
                                      const gchar *tool_name, const gchar *json_path,
                                      BtImportStats *stats, GError **error);

/* Claude Code / Codex .jsonl session log -> inbox/import/<tool>/YYYY-MM-DD-<slug>.md */
gboolean bt_import_session_jsonl(const BtVault *vault, BtIndex *index, const gchar *tool_name,
                                 const gchar *jsonl_path, BtImportStats *stats, GError **error);

/* Safe filename slug derived from title (never produces traversal or path seps). */
gchar *bt_importer_slug(const gchar *title);

G_END_DECLS

#endif
