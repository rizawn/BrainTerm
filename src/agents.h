/* agents.h — AI CLI detection, fixed-form launcher, vault-template wiring (§6.D5, §7.5, §8.1). */
#ifndef BT_AGENTS_H
#define BT_AGENTS_H

#include <glib.h>

#include "bterror.h"
#include "vault.h"

G_BEGIN_DECLS

typedef enum {
  BT_AGENT_CLAUDE = 0,
  BT_AGENT_CODEX  = 1,
  BT_AGENT_GEMINI = 2,
  BT_AGENT_COUNT  = 3
} BtAgentKind;

typedef struct {
  BtAgentKind kind;
  const gchar *id;          /* "claude-code", "codex", "gemini" */
  const gchar *cli_name;    /* "claude", "codex", "gemini" */
  const gchar *label;       /* "Claude here", "Codex here", "Gemini here" */
  gchar *resolved_path;     /* NULL if not detected */
} BtAgentInfo;

typedef struct {
  BtAgentInfo agents[BT_AGENT_COUNT];
} BtAgentRegistry;

void bt_agents_detect(BtAgentRegistry *reg);
void bt_agents_clear(BtAgentRegistry *reg);
const BtAgentInfo *bt_agents_get(const BtAgentRegistry *reg, BtAgentKind kind);

/* Composes the fixed-form launch keystroke (§6.D5, §8.1):
 *   <cli> "Read <sanitized-title> as context."\r
 * Neutralizes quotes, backticks, $, \, CR/LF so hostile note titles NEVER
 * cause shell metacharacter interpretation. */
gchar *bt_agents_format_launch_cmd(BtAgentKind kind, const gchar *note_title);

/* Composes the fixed-form resume command (§6.D6). */
gchar *bt_agents_format_resume_cmd(const gchar *tool_id, const gchar *session_id);

/* Applies vault-template/ files (AGENTS.md, CLAUDE.md, .gemini/settings.json,
 * .gitignore) to vault via vault_atomic_write(). NEVER overwrites an existing
 * AGENTS.md; sets *agents_md_differs = TRUE if an existing AGENTS.md differs. */
gboolean bt_agents_apply_vault_template(const BtVault *vault, gboolean *agents_md_differs,
                                        GError **error);

/* Returns the verbatim Appendix B AGENTS.md constitution. */
const gchar *bt_agents_template_agents_md(void);

G_END_DECLS

#endif
