/* createnote.h — Headless core note creation planner, name validator, template & commit API (Phase L3). */
#ifndef BT_CREATENOTE_H
#define BT_CREATENOTE_H

#include <glib.h>

#include "linkresolve.h"
#include "vault.h"

G_BEGIN_DECLS

typedef enum {
  BT_CREATE_PLAN_OK = 0,
  BT_CREATE_PLAN_EXISTS,
  BT_CREATE_PLAN_NOT_A_NOTE,
  BT_CREATE_PLAN_INVALID_NAME,
  BT_CREATE_PLAN_OUTSIDE_VAULT,
  BT_CREATE_PLAN_PLACEHOLDER
} BtCreatePlanResult;

typedef enum {
  BT_CREATE_COMMIT_OK = 0,
  BT_CREATE_COMMIT_EXISTS,
  BT_CREATE_COMMIT_BLOCKED,
  BT_CREATE_COMMIT_IO_ERROR
} BtCreateCommitResult;

typedef struct {
  BtCreatePlanResult result;
  gchar *target_relpath;          /* normalized NFC vault-relative path with .md/.markdown */
  gchar *note_name;               /* target basename without folder or note extension */
  gchar *display;                 /* alias if provided, else note_name */
  gchar *anchor;                  /* optional trailing #heading without '#', or NULL */
  gchar *block_id;                /* optional trailing ^blockid without '^', or NULL */
  gchar *reason;                  /* non-OK reason string */
  GPtrArray *missing_folders;     /* gchar* vault-relative folders missing on disk */
  gboolean has_folder_prefix;     /* TRUE if raw target specified folder(s) or needs folder creation */
  gboolean needs_folder_create;   /* TRUE if missing_folders->len > 0 */
  gboolean has_other_same_basename; /* TRUE when other notes share the same stem (CR12/Q-4) */
  gboolean stale_index;           /* TRUE when disk file existed despite being absent in snapshot */
} BtCreatePlan;

#define BT_CREATENOTE_MAX_SEGMENT_BYTES 240u
#define BT_CREATENOTE_MAX_PATH_BYTES 1024u

/* Validates a single path segment (folder or filename). Returns FALSE and sets out_reason
 * on forbidden chars, control chars, trailing '.'/' ', reserved Windows names, or >240 bytes. */
gboolean bt_createnote_validate_name(const char *segment, gchar **out_reason);

/* Pure read-only planner enforcing §6.1.3 Rules 1..12. Never writes to the vault. */
BtCreatePlan *bt_createnote_plan(const BtVault *vault, const BtIndexSnapshot *snap,
                                 const char *source_note, const char *raw_target);
BtCreatePlan *bt_createnote_plan_with_syntax(const BtVault *vault, const BtIndexSnapshot *snap,
                                             const char *source_note, const char *raw_target,
                                             BtLinkSyntax syntax);
void bt_createnote_plan_free(BtCreatePlan *plan);

/* Exact status-bar message per §6.1.7 / Appendix A (newly allocated, or NULL for PLACEHOLDER). */
gchar *bt_createnote_plan_status_message(const BtCreatePlan *plan);

const char *bt_createnote_plan_result_name(BtCreatePlanResult result);
const char *bt_createnote_commit_result_name(BtCreateCommitResult result);

G_END_DECLS

#endif
