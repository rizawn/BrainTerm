/* search.c — Ask bar mode detection, command registry matching, Ask-in-agent composer. */
#include "search.h"
#include "noteutil.h"

#include <string.h>

static const BtCommandSpec COMMANDS[] = {
  {">rebuild index",      "app.rebuild-index",   "Wipe L1 SQLite cache and rebuild from L0 files"},
  {">checkpoint",         "app.checkpoint",      "Take a manual git checkpoint of the vault"},
  {">open terminal here", "app.term-spawn",      "Open a new terminal tab in the vault"},
  {">rescan vault",       "app.rescan-vault",    "Rescan vault files on disk"},
  {">save",               "app.save-active",     "Save the active note atomically"},
  {">quick open",         "app.quick-open",      "Fuzzy-open a note (Ctrl+P)"},
  {">sessions",           "app.toggle-sessions", "Toggle the Agent Sessions panel"},
  {">toggle terminal",    "app.toggle-terminal", "Show or hide the terminal pane"},
  {">claude here",        "app.launch-claude",   "Launch Claude Code in the vault"},
  {">codex here",         "app.launch-codex",    "Launch Codex CLI in the vault"},
  {">gemini here",        "app.launch-gemini",   "Launch Gemini CLI in the vault"},
  {">audit index",        "app.audit-index",     "Differential audit of files vs body_hash (R3)"},
  {">organize vault",     "app.organize-now",    "Run auto-organize batch now (M6)"},
  {">toggle graph",       "app.toggle-graph",    "Show or hide the 2D Brain Graph (M7)"},
};

BtAskMode bt_search_detect_mode(const gchar *input, const gchar **trimmed_query)
{
  if (input == NULL) {
    if (trimmed_query) *trimmed_query = "";
    return BT_ASK_MODE_SEARCH;
  }
  while (*input == ' ' || *input == '\t')
    input++;
  if (input[0] == '>') {
    const gchar *q = input + 1;
    while (*q == ' ')
      q++;
    if (trimmed_query) *trimmed_query = q;
    return BT_ASK_MODE_COMMAND;
  }
  if (input[0] == '?') {
    const gchar *q = input + 1;
    while (*q == ' ')
      q++;
    if (trimmed_query) *trimmed_query = q;
    return BT_ASK_MODE_ASK;
  }
  gsize len = strlen(input);
  while (len > 0 && (input[len - 1] == ' ' || input[len - 1] == '\t'))
    len--;
  if (trimmed_query) *trimmed_query = input;
  if (len > 0 && input[len - 1] == '?')
    return BT_ASK_MODE_ASK;
  return BT_ASK_MODE_SEARCH;
}

GPtrArray *bt_search_match_commands(const gchar *query)
{
  GPtrArray *out = g_ptr_array_new();
  const gchar *q = query ? query : "";
  for (guint i = 0; i < G_N_ELEMENTS(COMMANDS); i++) {
    if (q[0] == '\0' || bt_fuzzy_score(q, COMMANDS[i].label + 1) > 0)
      g_ptr_array_add(out, (gpointer)&COMMANDS[i]);
  }
  return out;
}

static void append_sanitized(GString *dst, const gchar *src)
{
  for (const gchar *p = src ? src : ""; *p != '\0'; p++) {
    guchar c = (guchar)*p;
    if (c < 0x20 || c == 0x7F || c == '"' || c == '`' || c == '$' || c == '\\' || c == ';' ||
        c == '|' || c == '&' || c == '<' || c == '>' || c == '(' || c == ')')
      g_string_append_c(dst, ' ');
    else
      g_string_append_c(dst, (gchar)c);
  }
}

gchar *bt_search_compose_ask_agent_cmd(const gchar *question, const GPtrArray *hits,
                                       guint max_refs)
{
  GString *s = g_string_new("claude \"Answer using ");
  guint count = hits ? MIN(hits->len, max_refs > 0 ? max_refs : 5u) : 0;
  for (guint i = 0; i < count; i++) {
    const BtSearchHit *h = g_ptr_array_index(hits, i);
    g_string_append(s, "[[");
    append_sanitized(s, h->title && h->title[0] ? h->title : h->path);
    g_string_append(s, "]] ");
  }
  g_string_append(s, "as evidence: ");
  append_sanitized(s, question);
  g_string_append(s, "\"\r");
  return g_string_free(s, FALSE);
}
