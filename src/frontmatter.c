/* frontmatter.c — libyaml frontmatter parser & serializer (§7.1, R9). */
#include "frontmatter.h"

#include <string.h>
#include <yaml.h>

static void unknown_free(gpointer p)
{
  BtUnknownField *u = p;
  if (u == NULL)
    return;
  g_free(u->key);
  g_free(u->raw_line);
  g_free(u);
}

gchar *bt_fnv1a_hex(const gchar *data, gsize len)
{
  guint64 h = G_GUINT64_CONSTANT(14695981039346656037);
  for (gsize i = 0; i < len; i++) {
    h ^= (guint64)(guchar)data[i];
    h *= G_GUINT64_CONSTANT(1099511628211);
  }
  return g_strdup_printf("%016" G_GINT64_MODIFIER "x", h);
}

gint64 bt_parse_rfc3339_ms(const gchar *iso)
{
  if (iso == NULL || iso[0] == '\0')
    return 0;
  GDateTime *dt = g_date_time_new_from_iso8601(iso, NULL);
  if (dt == NULL) {
    /* Try date-only YYYY-MM-DD */
    gchar *with_t = g_strconcat(iso, "T00:00:00Z", NULL);
    dt = g_date_time_new_from_iso8601(with_t, NULL);
    g_free(with_t);
  }
  if (dt == NULL)
    return 0;
  gint64 ms = g_date_time_to_unix(dt) * 1000 + (g_date_time_get_microsecond(dt) / 1000);
  g_date_time_unref(dt);
  return ms;
}

static gboolean is_known_key(const gchar *k)
{
  return strcmp(k, "id") == 0 || strcmp(k, "title") == 0 || strcmp(k, "created") == 0 ||
         strcmp(k, "updated") == 0 || strcmp(k, "source") == 0 || strcmp(k, "agent") == 0 ||
         strcmp(k, "tags") == 0 || strcmp(k, "tags_ai") == 0 || strcmp(k, "links") == 0;
}

/* Locate closing '---' line. Returns FALSE if no valid frontmatter block. */
static gboolean find_frontmatter_bounds(const gchar *doc, gsize len, gsize *yaml_start,
                                        gsize *yaml_len, gsize *body_off)
{
  if (doc == NULL || len < 4)
    return FALSE;
  gsize pos = 0;
  if (len >= 3 && (guchar)doc[0] == 0xEF && (guchar)doc[1] == 0xBB && (guchar)doc[2] == 0xBF)
    pos = 3; /* UTF-8 BOM */
  if (pos + 3 > len || memcmp(doc + pos, "---", 3) != 0)
    return FALSE;
  pos += 3;
  while (pos < len && (doc[pos] == ' ' || doc[pos] == '\t'))
    pos++;
  if (pos < len && doc[pos] == '\r')
    pos++;
  if (pos >= len || doc[pos] != '\n')
    return FALSE;
  pos++;
  gsize start = pos;

  while (pos < len) {
    gsize line_start = pos;
    while (pos < len && doc[pos] != '\n')
      pos++;
    gsize line_end = pos;
    if (line_end > line_start && doc[line_end - 1] == '\r')
      line_end--;
    gsize llen = line_end - line_start;
    if (llen >= 3 && memcmp(doc + line_start, "---", 3) == 0) {
      gboolean trailing_ok = TRUE;
      for (gsize i = 3; i < llen; i++)
        if (doc[line_start + i] != ' ' && doc[line_start + i] != '\t')
          trailing_ok = FALSE;
      if (trailing_ok) {
        *yaml_start = start;
        *yaml_len = line_start - start;
        *body_off = (pos < len && doc[pos] == '\n') ? (pos + 1) : pos;
        return TRUE;
      }
    }
    if (pos < len)
      pos++;
  }
  return FALSE;
}

static gboolean is_yaml_null(const gchar *s)
{
  return s == NULL || s[0] == '\0' || strcmp(s, "null") == 0 || strcmp(s, "~") == 0;
}

static void extract_unknown_raw_lines(const gchar *yaml_text, gsize yaml_len, GPtrArray *unknown)
{
  gchar *copy = g_strndup(yaml_text, yaml_len);
  gchar **lines = g_strsplit(copy, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    gchar *line = lines[i];
    gsize ll = strlen(line);
    if (ll > 0 && line[ll - 1] == '\r')
      line[ll - 1] = '\0';
    if (line[0] == '\0' || line[0] == '#' || line[0] == ' ' || line[0] == '\t' || line[0] == '-')
      continue;
    const gchar *colon = strchr(line, ':');
    if (colon == NULL)
      continue;
    gchar *k = g_strndup(line, (gsize)(colon - line));
    g_strstrip(k);
    if (k[0] != '\0' && !is_known_key(k)) {
      BtUnknownField *u = g_new0(BtUnknownField, 1);
      u->key = g_strdup(k);
      u->raw_line = g_strdup(line);
      g_ptr_array_add(unknown, u);
    }
    g_free(k);
  }
  g_strfreev(lines);
  g_free(copy);
}

static void read_string_list(yaml_document_t *ydoc, yaml_node_t *val, GPtrArray *dst)
{
  if (val == NULL)
    return;
  if (val->type == YAML_SEQUENCE_NODE) {
    for (yaml_node_item_t *it = val->data.sequence.items.start; it < val->data.sequence.items.top;
         it++) {
      yaml_node_t *item = yaml_document_get_node(ydoc, *it);
      if (item != NULL && item->type == YAML_SCALAR_NODE) {
        gchar *s = g_strndup((const gchar *)item->data.scalar.value, item->data.scalar.length);
        if (s[0] != '\0')
          g_ptr_array_add(dst, s);
        else
          g_free(s);
      }
    }
  } else if (val->type == YAML_SCALAR_NODE) {
    gchar *s = g_strndup((const gchar *)val->data.scalar.value, val->data.scalar.length);
    if (!is_yaml_null(s))
      g_ptr_array_add(dst, s);
    else
      g_free(s);
  }
}

BtFrontmatter *bt_frontmatter_parse(const gchar *doc, gsize len)
{
  BtFrontmatter *fm = g_new0(BtFrontmatter, 1);
  fm->tags = g_ptr_array_new_with_free_func(g_free);
  fm->tags_ai = g_ptr_array_new_with_free_func(g_free);
  fm->links = g_ptr_array_new_with_free_func(g_free);
  fm->unknown = g_ptr_array_new_with_free_func(unknown_free);
  fm->source = g_strdup("human");

  gsize ystart = 0, ylen = 0, boff = 0;
  if (!find_frontmatter_bounds(doc, len, &ystart, &ylen, &boff))
    return fm;

  fm->has_frontmatter = TRUE;
  fm->body_offset = boff;

  yaml_parser_t parser;
  yaml_document_t ydoc;
  if (!yaml_parser_initialize(&parser)) {
    fm->parse_error = TRUE;
    return fm;
  }
  yaml_parser_set_input_string(&parser, (const unsigned char *)(doc + ystart), ylen);
  if (!yaml_parser_load(&parser, &ydoc)) {
    fm->parse_error = TRUE;
    yaml_parser_delete(&parser);
    return fm;
  }

  yaml_node_t *root = yaml_document_get_root_node(&ydoc);
  if (root != NULL && root->type != YAML_MAPPING_NODE) {
    fm->parse_error = TRUE;
  } else if (root != NULL) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (yaml_node_pair_t *pair = root->data.mapping.pairs.start;
         pair < root->data.mapping.pairs.top; pair++) {
      yaml_node_t *kn = yaml_document_get_node(&ydoc, pair->key);
      yaml_node_t *vn = yaml_document_get_node(&ydoc, pair->value);
      if (kn == NULL || kn->type != YAML_SCALAR_NODE)
        continue;
      gchar *k = g_strndup((const gchar *)kn->data.scalar.value, kn->data.scalar.length);
      if (g_hash_table_contains(seen, k)) {
        /* Duplicate key is a normative parse error (§7.1). */
        fm->parse_error = TRUE;
        g_free(k);
        continue;
      }
      g_hash_table_add(seen, g_strdup(k));

      gchar *sv = (vn && vn->type == YAML_SCALAR_NODE)
                      ? g_strndup((const gchar *)vn->data.scalar.value, vn->data.scalar.length)
                      : NULL;

      if (strcmp(k, "id") == 0 && sv && !is_yaml_null(sv)) {
        g_free(fm->id);
        fm->id = g_strdup(sv);
      } else if (strcmp(k, "title") == 0 && sv && !is_yaml_null(sv)) {
        g_free(fm->title);
        fm->title = g_strdup(sv);
      } else if (strcmp(k, "created") == 0 && sv && !is_yaml_null(sv)) {
        g_free(fm->created_str);
        fm->created_str = g_strdup(sv);
        fm->created_ms = bt_parse_rfc3339_ms(sv);
      } else if (strcmp(k, "updated") == 0 && sv && !is_yaml_null(sv)) {
        g_free(fm->updated_str);
        fm->updated_str = g_strdup(sv);
        fm->updated_ms = bt_parse_rfc3339_ms(sv);
      } else if (strcmp(k, "source") == 0 && sv) {
        if (strcmp(sv, "human") == 0 || strcmp(sv, "ai") == 0 || strcmp(sv, "import") == 0) {
          g_free(fm->source);
          fm->source = g_strdup(sv);
        }
      } else if (strcmp(k, "agent") == 0 && sv && !is_yaml_null(sv)) {
        g_free(fm->agent);
        fm->agent = g_strdup(sv);
      } else if (strcmp(k, "tags") == 0) {
        read_string_list(&ydoc, vn, fm->tags);
      } else if (strcmp(k, "tags_ai") == 0) {
        read_string_list(&ydoc, vn, fm->tags_ai);
      } else if (strcmp(k, "links") == 0) {
        read_string_list(&ydoc, vn, fm->links);
      }

      g_free(sv);
      g_free(k);
    }
    g_hash_table_destroy(seen);
    extract_unknown_raw_lines(doc + ystart, ylen, fm->unknown);
  }

  yaml_document_delete(&ydoc);
  yaml_parser_delete(&parser);
  return fm;
}

void bt_frontmatter_free(BtFrontmatter *fm)
{
  if (fm == NULL)
    return;
  g_free(fm->id);
  g_free(fm->title);
  g_free(fm->created_str);
  g_free(fm->updated_str);
  g_free(fm->source);
  g_free(fm->agent);
  g_ptr_array_unref(fm->tags);
  g_ptr_array_unref(fm->tags_ai);
  g_ptr_array_unref(fm->links);
  g_ptr_array_unref(fm->unknown);
  g_free(fm);
}

static void append_yaml_quoted(GString *s, const gchar *val)
{
  g_string_append_c(s, '"');
  for (const gchar *p = val ? val : ""; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c(s, '\\');
    g_string_append_c(s, *p);
  }
  g_string_append_c(s, '"');
}

static void append_flow_list(GString *s, const gchar *key, const GPtrArray *arr, gboolean quote)
{
  g_string_append_printf(s, "%s: [", key);
  for (guint i = 0; i < arr->len; i++) {
    if (i > 0)
      g_string_append(s, ", ");
    const gchar *v = g_ptr_array_index(arr, i);
    if (quote)
      append_yaml_quoted(s, v);
    else
      g_string_append(s, v);
  }
  g_string_append(s, "]\n");
}

gchar *bt_frontmatter_serialize(const BtFrontmatter *fm, const gchar *body)
{
  g_return_val_if_fail(fm != NULL, NULL);
  GString *s = g_string_new("---\n");
  if (fm->id)
    g_string_append_printf(s, "id: %s\n", fm->id);
  if (fm->title) {
    g_string_append(s, "title: ");
    append_yaml_quoted(s, fm->title);
    g_string_append_c(s, '\n');
  }
  if (fm->created_str)
    g_string_append_printf(s, "created: %s\n", fm->created_str);
  if (fm->updated_str)
    g_string_append_printf(s, "updated: %s\n", fm->updated_str);
  g_string_append_printf(s, "source: %s\n", fm->source ? fm->source : "human");
  g_string_append_printf(s, "agent: %s\n", fm->agent ? fm->agent : "null");
  append_flow_list(s, "tags", fm->tags, FALSE);
  append_flow_list(s, "tags_ai", fm->tags_ai, FALSE);
  append_flow_list(s, "links", fm->links, TRUE);
  for (guint i = 0; i < fm->unknown->len; i++) {
    const BtUnknownField *u = g_ptr_array_index(fm->unknown, i);
    g_string_append_printf(s, "%s\n", u->raw_line);
  }
  g_string_append(s, "---\n");
  if (body != NULL)
    g_string_append(s, body);
  return g_string_free(s, FALSE);
}
