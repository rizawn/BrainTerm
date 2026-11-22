/* noteutil.h — small pure helpers used by the editor UI: fuzzy matching for
 * Ctrl+P and wiki-link hit-testing/resolution for link clicks (§6.A2).
 * Wiki-links are not CommonMark; this is bracket scanning on one line, not a
 * Markdown parser (body extraction uses MD4C from M4 on). */
#ifndef BT_NOTEUTIL_H
#define BT_NOTEUTIL_H

#include <glib.h>

G_BEGIN_DECLS

/* -1 if query is not a (case-insensitive) subsequence of candidate;
 * otherwise a positive score, higher is better. Empty query scores 0. */
gint bt_fuzzy_score(const gchar *query, const gchar *candidate);

/* If byte_offset lies inside a [[...]] on line, returns the link target with
 * any |alias and #heading removed and whitespace trimmed; else NULL. */
gchar *bt_wikilink_at(const gchar *line, gsize byte_offset);

/* Finds the note a target refers to: exact relpath (sans .md) first, then
 * basename (sans .md), case-insensitively. Returns a copy or NULL. */
gchar *bt_wikilink_find_note(const GPtrArray *notes, const gchar *target);

/* Vault-relative path for creating a note from a target ("x" -> "x.md").
 * The result still MUST go through vault_resolve(). */
gchar *bt_wikilink_new_note_path(const gchar *target);

G_END_DECLS

#endif
