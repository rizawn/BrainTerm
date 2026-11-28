/* search.h — Ask bar logic: mode detection, command autocomplete, Ask-in-agent (§6.B4, P6). */
#ifndef BT_SEARCH_H
#define BT_SEARCH_H

#include <glib.h>

#include "index.h"

G_BEGIN_DECLS

typedef enum {
  BT_ASK_MODE_SEARCH,
  BT_ASK_MODE_ASK,
  BT_ASK_MODE_COMMAND
} BtAskMode;

typedef struct {
  const gchar *label;       /* e.g. ">rebuild index" */
  const gchar *action_name; /* e.g. "app.rebuild-index" */
  const gchar *description;
} BtCommandSpec;

/* Auto-detects mode: '>' prefix -> COMMAND; '?' prefix or trailing '?' -> ASK; else SEARCH. */
BtAskMode bt_search_detect_mode(const gchar *input, const gchar **trimmed_query);

/* Returns matching BtCommandSpec* (borrowed pointers in a newly allocated GPtrArray). */
GPtrArray *bt_search_match_commands(const gchar *query);

/* Composes the visible fixed-form terminal command for Ask mode (§6.B4, §8.1):
 *   <cli> "Answer using [[Note1]] [[Note2]]: <sanitized question>"\r
 * Never builds a chat UI (P6) and never leaves raw shell metacharacters. */
gchar *bt_search_compose_ask_agent_cmd(const gchar *question, const GPtrArray *hits,
                                       guint max_refs);
gchar *bt_search_compose_ask_agent_cmd_for(const gchar *cli_name, const gchar *question,
                                           const GPtrArray *hits, guint max_refs);

G_END_DECLS

#endif
