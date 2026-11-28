/* importer.c — Importers for ChatGPT, Claude, Claude Code, Codex, Gemini (§7.4, P3). */
#include "importer.h"
#include "frontmatter.h"

#include <cjson/cJSON.h>
#include <glib/gstdio.h>
#include <string.h>

#define BT_IMPORT_MAX_FILE_BYTES (64u * 1024u * 1024u)

gchar *bt_importer_slug(const gchar *title)
{
  if (title == NULL || title[0] == '\0')
    return g_strdup("untitled");
  GString *s = g_string_new(NULL);
  gboolean last_dash = TRUE;
  for (const gchar *p = title; *p != '\0' && s->len < 48; p++) {
    guchar c = (guchar)*p;
    if (g_ascii_isalnum(c)) {
      g_string_append_c(s, g_ascii_tolower((gchar)c));
      last_dash = FALSE;
    } else if (!last_dash) {
      g_string_append_c(s, '-');
      last_dash = TRUE;
    }
  }
  while (s->len > 0 && s->str[s->len - 1] == '-')
    g_string_truncate(s, s->len - 1);
  if (s->len == 0) {
    g_string_free(s, TRUE);
    gchar *h = bt_fnv1a_hex(title, strlen(title));
    gchar *res = g_strdup_printf("untitled-%.6s", h);
    g_free(h);
    return res;
  }
  return g_string_free(s, FALSE);
}

static gboolean read_import_file_bounded(const gchar *path, gchar **out_raw, GError **error)
{
  GStatBuf st_file;
  if (g_stat(path, &st_file) == 0 &&
      (st_file.st_size < 0 || (guint64)st_file.st_size > BT_IMPORT_MAX_FILE_BYTES)) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID,
                "Import file '%s' exceeds 64 MiB safety limit", path);
    return FALSE;
  }
  gsize raw_len = 0;
  if (!g_file_get_contents(path, out_raw, &raw_len, error))
    return FALSE;
  if (raw_len > BT_IMPORT_MAX_FILE_BYTES) {
    g_clear_pointer(out_raw, g_free);
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID,
                "Import file '%s' exceeds 64 MiB safety limit", path);
    return FALSE;
  }
  return TRUE;
}

static gchar *format_date_ymd(gint64 unix_sec)
{
  GDateTime *dt = unix_sec > 0 ? g_date_time_new_from_unix_utc(unix_sec)
                               : g_date_time_new_now_utc();
  gchar *ymd = g_date_time_format(dt, "%Y-%m-%d");
  g_date_time_unref(dt);
  return ymd;
}

static gchar *format_iso_utc(gint64 unix_sec)
{
  GDateTime *dt = unix_sec > 0 ? g_date_time_new_from_unix_utc(unix_sec)
                               : g_date_time_new_now_utc();
  gchar *iso = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%SZ");
  g_date_time_unref(dt);
  return iso;
}

static void append_yaml_escaped(GString *dst, const gchar *s)
{
  for (const gchar *p = s ? s : ""; *p != '\0'; p++) {
    guchar c = (guchar)*p;
    if (c == '\\')
      g_string_append(dst, "\\\\");
    else if (c == '"')
      g_string_append(dst, "\\\"");
    else if (c == '\n')
      g_string_append(dst, "\\n");
    else if (c == '\r')
      g_string_append(dst, "\\r");
    else if (c == '\t')
      g_string_append(dst, "\\t");
    else if (c < 0x20 || c == 0x7F)
      g_string_append_c(dst, ' ');
    else
      g_string_append_c(dst, (gchar)c);
  }
}

static void append_h1_safe(GString *dst, const gchar *s)
{
  for (const gchar *p = s ? s : ""; *p != '\0'; p++) {
    guchar c = (guchar)*p;
    if (c < 0x20 || c == 0x7F)
      g_string_append_c(dst, ' ');
    else
      g_string_append_c(dst, (gchar)c);
  }
}

static gboolean write_imported_note(const BtVault *vault, BtIndex *index, const gchar *tool_name,
                                    const gchar *title, gint64 created_sec,
                                    const gchar *human_turns, const gchar *ai_turns,
                                    BtImportStats *st)
{
  gchar *safe_tool = bt_importer_slug(tool_name);
  gchar *slug = bt_importer_slug(title);
  gchar *ymd = format_date_ymd(created_sec);
  gchar *iso = format_iso_utc(created_sec);

  gchar *rel = g_strdup_printf("inbox/import/%s/%s-%s.md", safe_tool, ymd, slug);
  gchar *abs = vault_resolve(vault, rel, NULL);
  if (abs == NULL) {
    st->rejected++;
    g_free(rel);
    g_free(iso);
    g_free(ymd);
    g_free(slug);
    g_free(safe_tool);
    return FALSE;
  }

  /* Idempotency: skip if already imported. */
  if (g_file_test(abs, G_FILE_TEST_EXISTS)) {
    st->skipped++;
    g_free(abs);
    g_free(rel);
    g_free(iso);
    g_free(ymd);
    g_free(slug);
    g_free(safe_tool);
    return TRUE;
  }
  g_free(abs);

  const gchar *safe_title = (title && title[0]) ? title : slug;
  gchar *h = bt_fnv1a_hex(rel, strlen(rel));
  GString *doc = g_string_new(NULL);
  g_string_append_printf(doc, "---\nid: bt-imp-%.6s\ntitle: \"", h);
  append_yaml_escaped(doc, safe_title);
  g_string_append_printf(
      doc,
      "\"\n"
      "created: %s\n"
      "updated: %s\n"
      "source: import\n"
      "agent: %s\n"
      "tags: []\n"
      "tags_ai: [%s]\n"
      "links: []\n"
      "---\n\n# ",
      iso, iso, safe_tool, safe_tool);
  append_h1_safe(doc, safe_title);
  g_string_append(doc, "\n\n");
  g_free(h);

  /* P3 structural separation: user prompts in body, AI responses in ## AI draft. */
  if (human_turns != NULL && human_turns[0] != '\0')
    g_string_append_printf(doc, "%s\n\n", human_turns);
  if (ai_turns != NULL && ai_turns[0] != '\0')
    g_string_append_printf(doc, "## AI draft\n\n%s\n", ai_turns);

  gboolean ok = vault_atomic_write(vault, rel, doc->str, doc->len, NULL, NULL);
  if (ok) {
    st->imported++;
    if (index != NULL)
      bt_index_upsert_note(index, vault, rel, NULL, NULL);
  } else {
    st->rejected++;
  }

  g_string_free(doc, TRUE);
  g_free(rel);
  g_free(iso);
  g_free(ymd);
  g_free(slug);
  g_free(safe_tool);
  return ok;
}

gboolean bt_import_conversations_json(const BtVault *vault, BtIndex *index,
                                      const gchar *tool_name, const gchar *json_path,
                                      BtImportStats *stats, GError **error)
{
  g_return_val_if_fail(vault != NULL && tool_name != NULL && json_path != NULL, FALSE);
  gchar *raw = NULL;
  if (!read_import_file_bounded(json_path, &raw, error))
    return FALSE;
  cJSON *root = cJSON_Parse(raw);
  g_free(raw);
  if (!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "Expected JSON array in '%s'", json_path);
    return FALSE;
  }

  BtImportStats st = {0};
  const cJSON *conv = NULL;
  cJSON_ArrayForEach(conv, root) {
    if (!cJSON_IsObject(conv))
      continue;
    const cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(conv, "title");
    if (!cJSON_IsString(jtitle))
      jtitle = cJSON_GetObjectItemCaseSensitive(conv, "name");
    const gchar *title = cJSON_IsString(jtitle) ? jtitle->valuestring : "Imported Chat";

    gint64 created_sec = 0;
    const cJSON *jct = cJSON_GetObjectItemCaseSensitive(conv, "create_time");
    if (cJSON_IsNumber(jct)) {
      created_sec = (gint64)jct->valuedouble;
    } else {
      const cJSON *jca = cJSON_GetObjectItemCaseSensitive(conv, "created_at");
      if (cJSON_IsString(jca))
        created_sec = bt_parse_rfc3339_ms(jca->valuestring) / 1000;
    }

    GString *human = g_string_new(NULL);
    GString *ai = g_string_new(NULL);

    /* Supports both Claude export (chat_messages[]) and simplified messages[]. */
    const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(conv, "chat_messages");
    if (!cJSON_IsArray(msgs))
      msgs = cJSON_GetObjectItemCaseSensitive(conv, "messages");
    if (cJSON_IsArray(msgs)) {
      const cJSON *m = NULL;
      cJSON_ArrayForEach(m, msgs) {
        const cJSON *sender = cJSON_GetObjectItemCaseSensitive(m, "sender");
        if (!cJSON_IsString(sender))
          sender = cJSON_GetObjectItemCaseSensitive(m, "role");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(m, "text");
        if (!cJSON_IsString(text))
          text = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!cJSON_IsString(text))
          continue;
        gboolean is_human = cJSON_IsString(sender) &&
                            (strcmp(sender->valuestring, "human") == 0 ||
                             strcmp(sender->valuestring, "user") == 0);
        if (is_human)
          g_string_append_printf(human, "> %s\n\n", text->valuestring);
        else
          g_string_append_printf(ai, "%s\n\n", text->valuestring);
      }
    }

    write_imported_note(vault, index, tool_name, title, created_sec, human->str, ai->str, &st);
    g_string_free(human, TRUE);
    g_string_free(ai, TRUE);
  }

  cJSON_Delete(root);
  if (stats != NULL)
    *stats = st;
  return TRUE;
}

gboolean bt_import_session_jsonl(const BtVault *vault, BtIndex *index, const gchar *tool_name,
                                 const gchar *jsonl_path, BtImportStats *stats, GError **error)
{
  g_return_val_if_fail(vault != NULL && tool_name != NULL && jsonl_path != NULL, FALSE);
  gchar *raw = NULL;
  if (!read_import_file_bounded(jsonl_path, &raw, error))
    return FALSE;

  gchar **lines = g_strsplit(raw, "\n", -1);
  g_free(raw);

  GString *human = g_string_new(NULL);
  GString *ai = g_string_new(NULL);
  gint64 first_ts = 0;
  gchar *title = g_path_get_basename(jsonl_path);
  gboolean has_nonempty_line = FALSE;
  guint parsed_objs = 0;

  for (guint i = 0; lines[i] != NULL; i++) {
    g_strstrip(lines[i]);
    if (lines[i][0] == '\0')
      continue;
    has_nonempty_line = TRUE;
    if (lines[i][0] != '{')
      continue;
    cJSON *obj = cJSON_Parse(lines[i]);
    if (!cJSON_IsObject(obj)) {
      cJSON_Delete(obj);
      continue;
    }
    parsed_objs++;
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(obj, "timestamp");
    if (first_ts == 0 && cJSON_IsString(ts))
      first_ts = bt_parse_rfc3339_ms(ts->valuestring) / 1000;
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(obj, "role");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(obj, "content");
    if (cJSON_IsString(content)) {
      gboolean is_user = cJSON_IsString(role) &&
                         (strcmp(role->valuestring, "user") == 0 ||
                          strcmp(role->valuestring, "human") == 0);
      if (is_user)
        g_string_append_printf(human, "> %s\n\n", content->valuestring);
      else
        g_string_append_printf(ai, "%s\n\n", content->valuestring);
    }
    cJSON_Delete(obj);
  }

  if (has_nonempty_line && parsed_objs == 0) {
    g_free(title);
    g_string_free(human, TRUE);
    g_string_free(ai, TRUE);
    g_strfreev(lines);
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID,
                "Expected JSONL object records in '%s'", jsonl_path);
    return FALSE;
  }

  BtImportStats st = {0};
  write_imported_note(vault, index, tool_name, title, first_ts, human->str, ai->str, &st);
  g_free(title);
  g_string_free(human, TRUE);
  g_string_free(ai, TRUE);
  g_strfreev(lines);
  if (stats != NULL)
    *stats = st;
  return TRUE;
}
