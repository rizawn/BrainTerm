/* src/mcp/main.c — Standalone read-only MCP stdio JSON-RPC 2.0 server (ADR-008).
 * Exposes search_memory, ask_memory, recent, get_note over vault/.bt/index.db
 * opened strictly READ-ONLY. Zero dependency on GTK4 or GUI process. */
#include <cjson/cJSON.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "embed.h"
#include "index.h"
#include "vault.h"

#define BT_MCP_MAX_LINE_BYTES (8u * 1024u * 1024u)

static void send_json(cJSON *obj)
{
  char *s = cJSON_PrintUnformatted(obj);
  if (s != NULL) {
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
  }
}

static void send_error(const cJSON *id, int code, const char *msg)
{
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  if (id != NULL)
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
  else
    cJSON_AddNullToObject(resp, "id");
  cJSON *err = cJSON_CreateObject();
  cJSON_AddNumberToObject(err, "code", code);
  cJSON_AddStringToObject(err, "message", msg);
  cJSON_AddItemToObject(resp, "error", err);
  send_json(resp);
  cJSON_Delete(resp);
}

static void send_result(const cJSON *id, cJSON *result)
{
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  if (id != NULL)
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
  else
    cJSON_AddNullToObject(resp, "id");
  cJSON_AddItemToObject(resp, "result", result);
  send_json(resp);
  cJSON_Delete(resp);
}

static cJSON *handle_tool_call(BtVault *vault, BtIndex *idx, const char *name, const cJSON *args)
{
  if (strcmp(name, "search_memory") == 0 || strcmp(name, "ask_memory") == 0) {
    const cJSON *jq = args ? cJSON_GetObjectItemCaseSensitive(args, "query") : NULL;
    const cJSON *jk = args ? cJSON_GetObjectItemCaseSensitive(args, "top_k") : NULL;
    const cJSON *jh = args ? cJSON_GetObjectItemCaseSensitive(args, "hybrid") : NULL;
    const char *q = cJSON_IsString(jq) ? jq->valuestring : "";
    guint k = cJSON_IsNumber(jk) ? (guint)CLAMP((int)jk->valuedouble, 1, 50) : 10u;
    gboolean want_hybrid = cJSON_IsBool(jh) && cJSON_IsTrue(jh) && bt_embed_count_cached(idx) > 0;

    GPtrArray *hits = want_hybrid ? bt_embed_hybrid_search(NULL, idx, q, k, TRUE, NULL)
                                  : bt_index_search_fts(idx, q, k, NULL);
    cJSON *arr = cJSON_CreateArray();
    for (guint i = 0; hits != NULL && i < hits->len; i++) {
      const BtSearchHit *h = g_ptr_array_index(hits, i);
      cJSON *item = cJSON_CreateObject();
      gchar *cite = g_strdup_printf("[[%s]]", h->title && h->title[0] ? h->title : h->path);
      cJSON_AddStringToObject(item, "citation", cite);
      cJSON_AddStringToObject(item, "path", h->path);
      cJSON_AddStringToObject(item, "title", h->title);
      cJSON_AddStringToObject(item, "snippet", h->snippet);
      cJSON_AddItemToArray(arr, item);
      g_free(cite);
    }
    g_clear_pointer(&hits, g_ptr_array_unref);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "mode",
                            strcmp(name, "ask_memory") == 0 ? "evidence"
                                                            : (want_hybrid ? "hybrid" : "search"));
    cJSON_AddItemToObject(res, "matches", arr);
    return res;
  }

  if (strcmp(name, "recent") == 0) {
    const cJSON *jl = args ? cJSON_GetObjectItemCaseSensitive(args, "limit") : NULL;
    guint lim = cJSON_IsNumber(jl) ? (guint)CLAMP((int)jl->valuedouble, 1, 100) : 10u;
    GPtrArray *notes = bt_index_list_recent_notes(idx, lim, NULL);
    cJSON *arr = cJSON_CreateArray();
    for (guint i = 0; notes != NULL && i < notes->len && i < lim; i++) {
      const BtNoteRow *r = g_ptr_array_index(notes, i);
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "id", r->id);
      cJSON_AddStringToObject(item, "path", r->path);
      cJSON_AddStringToObject(item, "title", r->title);
      cJSON_AddNumberToObject(item, "updated", (double)r->updated);
      cJSON_AddItemToArray(arr, item);
    }
    g_clear_pointer(&notes, g_ptr_array_unref);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "notes", arr);
    return res;
  }

  if (strcmp(name, "get_note") == 0) {
    const cJSON *jp = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    if (!cJSON_IsString(jp))
      return NULL;
    gchar *content = NULL;
    if (!vault_read(vault, jp->valuestring, &content, NULL, NULL))
      return NULL;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "path", jp->valuestring);
    cJSON_AddStringToObject(res, "content", content);
    g_free(content);
    return res;
  }

  if (strcmp(name, "backlinks") == 0) {
    const cJSON *jp = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    if (!cJSON_IsString(jp) || jp->valuestring[0] == '\0')
      return NULL;
    gchar *abs = vault_resolve(vault, jp->valuestring, NULL);
    if (abs == NULL)
      return NULL;
    g_free(abs);
    GPtrArray *bls = bt_index_list_backlinks(idx, jp->valuestring, NULL);
    cJSON *arr = cJSON_CreateArray();
    for (guint i = 0; bls != NULL && i < bls->len; i++) {
      const BtLinkResRow *r = g_ptr_array_index(bls, i);
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "src_path", r->src_path);
      cJSON_AddStringToObject(item, "raw_target", r->raw_target);
      cJSON_AddNumberToObject(item, "line_no", r->line_no);
      cJSON_AddItemToArray(arr, item);
    }
    g_clear_pointer(&bls, g_ptr_array_unref);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "path", jp->valuestring);
    cJSON_AddItemToObject(res, "backlinks", arr);
    return res;
  }

  return NULL; /* unknown or write tool -> refused */
}

static void add_schema_prop(cJSON *props, const char *key, const char *type)
{
  if (key == NULL || type == NULL)
    return;
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p, "type", type);
  cJSON_AddItemToObject(props, key, p);
}

static cJSON *make_tool_schema(const char *name, const char *desc, const char *req_prop,
                               const char *opt1_prop, const char *opt1_type,
                               const char *opt2_prop, const char *opt2_type)
{
  cJSON *t = cJSON_CreateObject();
  cJSON_AddStringToObject(t, "name", name);
  cJSON_AddStringToObject(t, "description", desc);
  cJSON *schema = cJSON_CreateObject();
  cJSON_AddStringToObject(schema, "type", "object");
  cJSON *props = cJSON_CreateObject();
  if (req_prop != NULL)
    add_schema_prop(props, req_prop, strcmp(req_prop, "limit") == 0 ? "integer" : "string");
  add_schema_prop(props, opt1_prop, opt1_type);
  add_schema_prop(props, opt2_prop, opt2_type);
  cJSON_AddItemToObject(schema, "properties", props);
  if (req_prop != NULL && strcmp(req_prop, "limit") != 0) {
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString(req_prop));
    cJSON_AddItemToObject(schema, "required", req);
  }
  cJSON_AddItemToObject(t, "inputSchema", schema);
  return t;
}

static gboolean read_json_line(FILE *fp, GString *buf, gboolean *out_too_large)
{
  *out_too_large = FALSE;
  g_string_truncate(buf, 0);
  int ch;
  while ((ch = fgetc(fp)) != EOF) {
    if (ch == '\n')
      return TRUE;
    if (buf->len < BT_MCP_MAX_LINE_BYTES)
      g_string_append_c(buf, (gchar)ch);
    else
      *out_too_large = TRUE;
  }
  return buf->len > 0;
}

int main(int argc, char **argv)
{
  const gchar *vault_path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--vault") == 0 && i + 1 < argc)
      vault_path = argv[++i];
  }
  if (vault_path == NULL) {
    fprintf(stderr, "usage: bt-mcp --vault <dir>\n");
    return 2;
  }

  GError *err = NULL;
  BtVault *vault = vault_open_readonly(vault_path, NULL, &err);
  if (vault == NULL) {
    fprintf(stderr, "bt-mcp: %s\n", err ? err->message : "cannot open vault");
    g_clear_error(&err);
    return 1;
  }
  gchar *db_path = g_build_filename(vault_root(vault), ".bt", "index.db", NULL);
  BtIndex *idx = bt_index_open_readonly(db_path, &err);
  g_free(db_path);
  if (idx == NULL) {
    fprintf(stderr, "bt-mcp: %s\n", err ? err->message : "cannot open index read-only");
    g_clear_error(&err);
    vault_free(vault);
    return 1;
  }

  GString *line = g_string_sized_new(4096);
  gboolean too_large = FALSE;
  while (read_json_line(stdin, line, &too_large)) {
    if (too_large) {
      send_error(NULL, -32600, "Request line exceeds 8 MiB limit");
      continue;
    }
    g_strstrip(line->str);
    if (line->str[0] == '\0')
      continue;
    cJSON *req = cJSON_Parse(line->str);
    if (!cJSON_IsObject(req)) {
      cJSON_Delete(req);
      send_error(NULL, -32700, "Parse error");
      continue;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");

    if (!cJSON_IsString(method)) {
      send_error(id, -32600, "Invalid Request");
    } else if (strcmp(method->valuestring, "initialize") == 0) {
      cJSON *res = cJSON_CreateObject();
      cJSON_AddStringToObject(res, "protocolVersion", "2024-11-05");
      cJSON *caps = cJSON_CreateObject();
      cJSON *tools_cap = cJSON_CreateObject();
      cJSON_AddBoolToObject(tools_cap, "listChanged", 0);
      cJSON_AddItemToObject(caps, "tools", tools_cap);
      cJSON_AddItemToObject(res, "capabilities", caps);
      cJSON *info = cJSON_CreateObject();
      cJSON_AddStringToObject(info, "name", "bt-mcp");
      cJSON_AddStringToObject(info, "version", "1.0.0");
      cJSON_AddItemToObject(res, "serverInfo", info);
      cJSON_AddBoolToObject(res, "readOnly", 1);
      send_result(id, res);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
      cJSON *res = cJSON_CreateObject();
      cJSON *tools = cJSON_CreateArray();
      cJSON_AddItemToArray(tools, make_tool_schema(
          "search_memory", "Full-text (and optional cached hybrid) search over indexed notes",
          "query", "top_k", "integer", "hybrid", "boolean"));
      cJSON_AddItemToArray(tools, make_tool_schema(
          "ask_memory", "Return top matching citations and snippets as read-only evidence",
          "query", "top_k", "integer", "hybrid", "boolean"));
      cJSON_AddItemToArray(tools, make_tool_schema(
          "recent", "List most recently updated notes in the vault",
          "limit", NULL, NULL, NULL, NULL));
      cJSON_AddItemToArray(tools, make_tool_schema(
          "get_note", "Read raw Markdown content of a vault-relative note path",
          "path", NULL, NULL, NULL, NULL));
      cJSON_AddItemToArray(tools, make_tool_schema(
          "backlinks", "List resolved incoming wikilinks pointing to a note path",
          "path", NULL, NULL, NULL, NULL));
      cJSON_AddItemToObject(res, "tools", tools);
      send_result(id, res);
    } else if (strcmp(method->valuestring, "tools/call") == 0 && cJSON_IsObject(params)) {
      const cJSON *tname = cJSON_GetObjectItemCaseSensitive(params, "name");
      const cJSON *targs = cJSON_GetObjectItemCaseSensitive(params, "arguments");
      if (!cJSON_IsString(tname)) {
        send_error(id, -32602, "Missing tool name");
      } else {
        cJSON *tres = handle_tool_call(vault, idx, tname->valuestring, targs);
        if (tres != NULL)
          send_result(id, tres);
        else
          send_error(id, -32601, "Tool not found or refused (bt-mcp is read-only)");
      }
    } else {
      send_error(id, -32601, "Method not found");
    }
    cJSON_Delete(req);
  }
  g_string_free(line, TRUE);

  bt_index_close(idx);
  vault_free(vault);
  return 0;
}
