/* organizer.h — Auto-organize memory engine with No-AI / CLI / Local-Model
 * backends, P4 human-lock enforcement, and ADR-006 batch checkpointing (§6.B3). */
#ifndef BT_ORGANIZER_H
#define BT_ORGANIZER_H

#include <glib.h>

#include "bterror.h"
#include "index.h"
#include "vault.h"

G_BEGIN_DECLS

typedef enum {
  BT_ORG_NO_AI = 0,       /* default: pure C rules, always available */
  BT_ORG_CLI = 1,         /* headless agent CLI (fixed argv, never shell) */
  BT_ORG_LOCAL_MODEL = 2  /* M8 llama.cpp hook (stub in M6) */
} BtOrgMode;

typedef struct {
  guint notes_processed;
  guint notes_updated;
  guint tags_proposed;
  gchar checkpoint_oid[41];
} BtOrgStats;

/* Optional custom callback for local-model backend (wired in M8). */
typedef GPtrArray *(*BtOrgModelFn)(const gchar *title, const gchar *body, gpointer user_data);

/* Runs one checkpointed organize batch over relpaths (or all vault notes if
 * relpaths == NULL). Refuses entire batch if git checkpoint fails (R10).
 * NEVER writes human tags or body text (P3/P4). */
gboolean bt_organizer_run_batch(const BtVault *vault, BtIndex *index,
                                const GPtrArray *relpaths, BtOrgMode mode,
                                const gchar *cli_override, BtOrgModelFn model_fn,
                                gpointer model_ud, BtOrgStats *stats, GError **error);

/* Human confirmation: promotes an AI tag from tags_ai -> tags (origin: human,
 * permanently locked against future AI writes). */
gboolean bt_organizer_promote_tag(const BtVault *vault, BtIndex *index, const gchar *relpath,
                                  const gchar *tag, GError **error);

/* Human dismissal: removes tag from tags_ai and records it as dismissed for
 * this note so no backend ever re-proposes it. */
gboolean bt_organizer_dismiss_tag(const BtVault *vault, BtIndex *index, const gchar *relpath,
                                  const gchar *tag, GError **error);

G_END_DECLS

#endif
