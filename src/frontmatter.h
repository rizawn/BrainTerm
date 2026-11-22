/* frontmatter.h — YAML frontmatter subset parser/serializer via libyaml (§7.1, ADR-003). */
#ifndef BT_FRONTMATTER_H
#define BT_FRONTMATTER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gchar *key;
  gchar *raw_line; /* preserved verbatim on serialize */
} BtUnknownField;

typedef struct {
  gboolean has_frontmatter;
  gboolean parse_error; /* duplicate key or malformed YAML (R9) */

  gchar *id;
  gchar *title;
  gchar *created_str; /* RFC 3339 original string */
  gchar *updated_str;
  gint64 created_ms;  /* unix ms */
  gint64 updated_ms;
  gchar *source;      /* "human" | "ai" | "import" (defaults to "human") */
  gchar *agent;       /* NULL or tool id */

  GPtrArray *tags;    /* gchar* human-owned tags (P4) */
  GPtrArray *tags_ai; /* gchar* AI-proposed tags */
  GPtrArray *links;   /* gchar* declared wiki-links */
  GPtrArray *aliases; /* gchar* note aliases */

  GPtrArray *unknown; /* BtUnknownField* preserved verbatim */

  gsize body_offset;  /* byte offset where Markdown body begins */
} BtFrontmatter;

/* Parses frontmatter at the top of a Markdown document using libyaml.
 * Always returns a valid BtFrontmatter (never NULL); check parse_error for R9. */
BtFrontmatter *bt_frontmatter_parse(const gchar *doc, gsize len);
void bt_frontmatter_free(BtFrontmatter *fm);

/* Serializes frontmatter + body back to a full Markdown document.
 * Preserves unknown keys verbatim and respects exact §7.1 key order. */
gchar *bt_frontmatter_serialize(const BtFrontmatter *fm, const gchar *body);

/* 64-bit FNV-1a hex hash used for fm_hash / body_hash (§7.2, §6.A5). */
gchar *bt_fnv1a_hex(const gchar *data, gsize len);

/* Parses an RFC 3339 timestamp to unix ms; returns 0 on failure. */
gint64 bt_parse_rfc3339_ms(const gchar *iso);

G_END_DECLS

#endif
