/* linkresolve.h — GTK-free clickable link scanner, resolver, and A.9 security policy (Phase L1).
 *
 * Pure core module inside libbt_core (zero GTK includes). Shared by:
 *   - Editor link parsing & click/hover/navigation (L2/L3)
 *   - SQLite link_resolutions table (0002_links_resolution.sql)
 *   - Brain Graph ghost node detection (graph.c)
 *   - Headless --selftest-links <vault> CLI
 */
#ifndef BT_LINKRESOLVE_H
#define BT_LINKRESOLVE_H

#include <glib.h>

#include "vault.h"

G_BEGIN_DECLS

typedef enum {
  BT_LINK_RESOLVED = 0,
  BT_LINK_AMBIGUOUS = 1,
  BT_LINK_UNRESOLVED = 2,
  BT_LINK_EXTERNAL = 3,
  BT_LINK_BLOCKED = 4,
  BT_LINK_PLACEHOLDER = 5,
} BtLinkStatus;

typedef enum {
  BT_LINK_KIND_NOTE = 0,
  BT_LINK_KIND_FILE = 1,
  BT_LINK_KIND_DIR = 2,
  BT_LINK_KIND_URL = 3,
  BT_LINK_KIND_SAME_NOTE = 4,
} BtLinkKind;

typedef enum {
  BT_LINK_BLOCK_NONE = 0,
  BT_LINK_BLOCK_ESCAPES_VAULT = 1,
  BT_LINK_BLOCK_UNSAFE_PATH = 2,
  BT_LINK_BLOCK_SCHEME_NOT_ALLOWED = 3,
  BT_LINK_BLOCK_TOO_LONG = 4,
} BtLinkBlockReason;

typedef enum {
  BT_LINK_SYNTAX_WIKILINK = 0,
  BT_LINK_SYNTAX_MARKDOWN = 1,
  BT_LINK_SYNTAX_BARE_URL = 2,
  BT_LINK_SYNTAX_FRONTMATTER = 3,
} BtLinkSyntax;

typedef struct {
  gsize start;        /* byte offset of full token */
  gsize end;          /* byte offset past full token */
  gsize target_start; /* byte offset of raw target */
  gsize target_end;   /* byte offset past raw target */
  guint line_no;      /* 1-based line number */
  BtLinkSyntax syntax;
  gchar *raw_target;
  gchar *md_label;    /* non-NULL for [label](dest) */
} BtLinkSpan;

typedef struct {
  BtLinkStatus status;
  BtLinkKind kind;
  BtLinkBlockReason block_reason;
  gchar *path;        /* vault-relative, '/' separators, original case; NULL if none */
  gchar **candidates; /* NULL-terminated ranked list for AMBIGUOUS (or NULL) */
  gchar *anchor;      /* heading without '#', or NULL */
  gchar *block_id;    /* block id without '^', or NULL */
  gchar *display;     /* alias if present, else target text */
  gchar *url;         /* for EXTERNAL */
} BtLinkResolution;

typedef struct BtIndexSnapshot BtIndexSnapshot;

/* Scans UTF-8 Markdown buffer for wikilinks, Markdown links, bare URLs, and
 * frontmatter links, excluding code blocks, inline code, HTML comments, and
 * math spans. Returns a GArray of BtLinkSpan sorted by start byte offset. */
GArray *bt_link_scan(const char *text, gsize len);

/* Incremental paragraph/block rescan covering [range_start, range_end). */
GArray *bt_link_scan_range(const char *text, gsize len, gsize range_start, gsize range_end,
                           gsize *out_block_start, gsize *out_block_end);

/* Binary search for a link span containing byte_offset. Returns shallow copy
 * pointers valid while spans is alive, OR FALSE if not found. */
gboolean bt_link_span_at(const GArray *spans, gsize byte_offset, BtLinkSpan *out_span);

/* Immutable snapshot builder / destructor. */
BtIndexSnapshot *bt_index_snapshot_new(void);
void bt_index_snapshot_free(BtIndexSnapshot *snap);
void bt_index_snapshot_set_case_sensitive(BtIndexSnapshot *snap, gboolean case_sensitive);
void bt_index_snapshot_set_vault(BtIndexSnapshot *snap, const BtVault *vault);
void bt_index_snapshot_set_fast_tag_mode(BtIndexSnapshot *snap, gboolean fast_mode);
void bt_index_snapshot_add_entry(BtIndexSnapshot *snap, const char *relpath, gboolean is_dir,
                                 const char *title, const char *const *aliases);
void bt_index_snapshot_finalize(BtIndexSnapshot *snap);

/* Builds an immutable snapshot by walking the vault tree read-only (no writes). */
BtIndexSnapshot *bt_index_snapshot_from_vault(const BtVault *vault);

/* Resolves raw_target from source_note against idx per §A.3.
 * Returns NULL if raw_target represents an empty non-link (e.g. [[]]). */
BtLinkResolution *bt_link_resolve(const BtIndexSnapshot *idx, const char *source_note,
                                  const char *raw_target);
void bt_link_resolution_free(BtLinkResolution *r);

/* --------------------------------------------------- A.9 security policy -- */

typedef enum {
  BT_LINK_OPEN_ALLOW = 0,
  BT_LINK_OPEN_CONFIRM_EXT = 1,
  BT_LINK_OPEN_CONFIRM_DIR = 2,
  BT_LINK_OPEN_CONFIRM_URL = 3,
  BT_LINK_OPEN_BLOCKED = 4,
} BtLinkOpenAction;

typedef struct {
  BtLinkOpenAction action;
  BtLinkBlockReason block_reason;
  gboolean has_userinfo;
  gchar *safe_display_url; /* user:pass@ stripped for EXTERNAL URLs */
  gchar *reason_msg;
} BtLinkOpenPolicy;

typedef gboolean (*BtLinkLauncherFn)(const char *target_or_url, gboolean is_uri, void *user_data);

void bt_link_check_open_policy(const BtLinkResolution *res, const BtVault *vault,
                               gboolean confirm_external_urls, BtLinkOpenPolicy *out_policy);
void bt_link_open_policy_clear(BtLinkOpenPolicy *policy);

/* Checks A.9 policy and only invokes launcher if action == ALLOW (or confirmed_by_user
 * for CONFIRM_* actions). NEVER invokes launcher when BLOCKED. */
gboolean bt_link_dispatch_open(const BtLinkResolution *res, const BtVault *vault,
                               gboolean confirmed_by_user, gboolean confirm_external_urls,
                               BtLinkLauncherFn launcher, void *user_data,
                               BtLinkOpenPolicy *out_policy);

const char *bt_link_status_name(BtLinkStatus status);
const char *bt_link_kind_name(BtLinkKind kind);
const char *bt_link_block_reason_message(BtLinkBlockReason reason);

/* Shared path/snapshot helpers used by core planner (libbt_core, GTK-free). */
gboolean bt_link_is_note_ext(const char *path);
gboolean bt_link_is_reserved_device_name(const char *seg);
gboolean bt_link_percent_decode_path(const char *in, gchar **out, gboolean *out_ctrl);
gboolean bt_index_snapshot_is_case_sensitive(const BtIndexSnapshot *snap);
gboolean bt_index_snapshot_find_relpath(const BtIndexSnapshot *snap, const char *relpath,
                                        gboolean *out_is_dir, gchar **out_actual_relpath);
gboolean bt_index_snapshot_has_other_basename(const BtIndexSnapshot *snap, const char *note_stem,
                                            const char *exclude_relpath);

/* Runs headless, read-only link scan + resolve audit on vault_dir (A.15). */
int bt_link_selftest_cli(const char *vault_dir);

G_END_DECLS

#endif
