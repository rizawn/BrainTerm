/* src/mcp/main.c — Standalone read-only MCP stdio JSON-RPC 2.0 server (ADR-008).
 * Exposes search_memory, ask_memory, recent, get_note over vault/.bt/index.db
 * opened strictly READ-ONLY. Zero dependency on GTK4 or GUI process. */
#include <cjson/cJSON.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index.h"
#include "vault.h"

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
    const cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
    const cJSON *jk = cJSON_GetObjectItemCaseSensitive(args, "top_k");
    const char *q = cJSON_IsString(jq) ? jq->valuestring : "";
    guint k = cJSON_IsNumber(jk) ? (guint)CLAMP((int)jk->valuedouble, 1, 50) : 10u;

    GPtrArray *hits = bt_index_search_fts(idx, q, k, NULL);
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
    cJSON_AddStringToObject(res, "mode", strcmp(name, "ask_memory") == 0 ? "evidence" : "search");
    cJSON_AddItemToObject(res, "matches", arr);
    return res;
  }

  if (strcmp(name, "recent") == 0) {
    const cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
    guint lim = cJSON_IsNumber(jl) ? (guint)CLAMP((int)jl->valuedouble, 1, 100) : 10u;
    GPtrArray *notes = bt_index_list_notes(idx, NULL);
    cJSON *arr = cJSON_CreateArray();
    for (guint i = 0; notes != NULL && i < notes->len && i < lim; i++) {
      const BtNoteRow *r = g_ptr_array_index(notes, i);
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "id", r->id);
      cJSON_AddStringToObject(item, "path", r->path);
      cJSON_AddStringToObject(item, "title", r->title);
      cJSON_AddItemToArray(arr, item);
    }
    g_clear_pointer(&notes, g_ptr_array_unref);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "notes", arr);
    return res;
  }

  if (strcmp(name, "get_note") == 0) {
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "path");
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

  return NULL; /* unknown or write tool -> refused */
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
  BtVault *vault = vault_open(vault_path, NULL, &err);
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

  char line[65536];
  while (fgets(line, sizeof(line), stdin) != NULL) {
    cJSON *req = cJSON_Parse(line);
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
      cJSON *info = cJSON_CreateObject();
      cJSON_AddStringToObject(info, "name", "bt-mcp");
      cJSON_AddStringToObject(info, "version", "0.1.0");
      cJSON_AddItemToObject(res, "serverInfo", info);
      cJSON_AddBoolToObject(res, "readOnly", 1);
      send_result(id, res);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
      cJSON *res = cJSON_CreateObject();
      cJSON *tools = cJSON_CreateArray();
      static const char *const names[] = {"search_memory", "ask_memory", "recent", "get_note"};
      for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", names[i]);
        cJSON_AddItemToArray(tools, t);
      }
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

  bt_index_close(idx);
  vault_free(vault);
  return 0;
}
