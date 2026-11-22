/* sessions.c — cJSON-backed session persistence in vault/.bt/sessions.json (M3). */
#include "sessions.h"

#include <cjson/cJSON.h>
#include <glib/gstdio.h>
#include <string.h>

struct BtSessionStore {
  gchar *vault_root;
  gchar *json_path;
  GPtrArray *items; /* BtSession* */
  guint seq;
};

void bt_session_free(gpointer session)
{
  BtSession *s = session;
  if (s == NULL)
    return;
  g_free(s->id);
  g_free(s->tool);
  g_free(s->cwd);
  g_free(s->resume_cmd);
  g_free(s->note_id);
  g_free(s);
}

gchar *bt_sessions_json_path(const gchar *vault_root)
{
  return g_build_filename(vault_root, ".bt", "sessions.json", NULL);
}

static gboolean save_store(BtSessionStore *store, GError **error)
{
  gchar *dir = g_path_get_dirname(store->json_path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);

  cJSON *arr = cJSON_CreateArray();
  for (guint i = 0; i < store->items->len; i++) {
    const BtSession *s = g_ptr_array_index(store->items, i);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", s->id);
    cJSON_AddStringToObject(obj, "tool", s->tool);
    cJSON_AddStringToObject(obj, "cwd", s->cwd);
    cJSON_AddNumberToObject(obj, "started", (double)s->started);
    if (s->ended > 0)
      cJSON_AddNumberToObject(obj, "ended", (double)s->ended);
    else
      cJSON_AddNullToObject(obj, "ended");
    cJSON_AddStringToObject(obj, "resume_cmd", s->resume_cmd ? s->resume_cmd : "");
    if (s->note_id != NULL)
      cJSON_AddStringToObject(obj, "note_id", s->note_id);
    else
      cJSON_AddNullToObject(obj, "note_id");
    cJSON_AddStringToObject(obj, "checkpoint_oid", s->checkpoint_oid);
    cJSON_AddItemToArray(arr, obj);
  }

  char *printed = cJSON_Print(arr);
  cJSON_Delete(arr);
  if (printed == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot serialize sessions.json");
    return FALSE;
  }

  GError *local = NULL;
  gboolean ok = g_file_set_contents(store->json_path, printed, -1, &local);
  free(printed);
  if (!ok) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot write sessions.json: %s", local->message);
    g_error_free(local);
  }
  return ok;
}

static void load_store(BtSessionStore *store)
{
  gchar *raw = NULL;
  if (!g_file_get_contents(store->json_path, &raw, NULL, NULL))
    return;
  cJSON *root = cJSON_Parse(raw);
  g_free(raw);
  if (!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return;
  }

  cJSON *el = NULL;
  cJSON_ArrayForEach(el, root) {
    if (!cJSON_IsObject(el))
      continue;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(el, "id");
    const cJSON *tool = cJSON_GetObjectItemCaseSensitive(el, "tool");
    const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(el, "cwd");
    const cJSON *started = cJSON_GetObjectItemCaseSensitive(el, "started");
    const cJSON *ended = cJSON_GetObjectItemCaseSensitive(el, "ended");
    const cJSON *resume = cJSON_GetObjectItemCaseSensitive(el, "resume_cmd");
    const cJSON *note = cJSON_GetObjectItemCaseSensitive(el, "note_id");
    const cJSON *oid = cJSON_GetObjectItemCaseSensitive(el, "checkpoint_oid");
    if (!cJSON_IsString(id) || !cJSON_IsString(tool) || !cJSON_IsString(cwd))
      continue;

    BtSession *s = g_new0(BtSession, 1);
    s->id = g_strdup(id->valuestring);
    s->tool = g_strdup(tool->valuestring);
    s->cwd = g_strdup(cwd->valuestring);
    s->started = cJSON_IsNumber(started) ? (gint64)started->valuedouble : 0;
    s->ended = cJSON_IsNumber(ended) ? (gint64)ended->valuedouble : 0;
    s->resume_cmd = cJSON_IsString(resume) ? g_strdup(resume->valuestring) : g_strdup("");
    s->note_id = cJSON_IsString(note) ? g_strdup(note->valuestring) : NULL;
    if (cJSON_IsString(oid))
      g_strlcpy(s->checkpoint_oid, oid->valuestring, BT_OID_HEX_LEN);
    g_ptr_array_add(store->items, s);
  }
  cJSON_Delete(root);
}

BtSessionStore *bt_sessions_open(const gchar *vault_root, GError **error)
{
  (void)error;
  g_return_val_if_fail(vault_root != NULL, NULL);
  BtSessionStore *store = g_new0(BtSessionStore, 1);
  store->vault_root = g_strdup(vault_root);
  store->json_path = bt_sessions_json_path(vault_root);
  store->items = g_ptr_array_new_with_free_func(bt_session_free);
  load_store(store);
  return store;
}

void bt_sessions_free(BtSessionStore *store)
{
  if (store == NULL)
    return;
  g_ptr_array_unref(store->items);
  g_free(store->json_path);
  g_free(store->vault_root);
  g_free(store);
}

const BtSession *bt_sessions_add(BtSessionStore *store, const gchar *tool, const gchar *cwd,
                                 const gchar *resume_cmd, const gchar *note_id,
                                 const gchar *checkpoint_oid, GError **error)
{
  g_return_val_if_fail(store != NULL && tool != NULL && cwd != NULL, NULL);
  gint64 now_ms = g_get_real_time() / 1000;
  store->seq++;

  BtSession *s = g_new0(BtSession, 1);
  s->id = g_strdup_printf("sess-%" G_GINT64_FORMAT "-%u", now_ms, store->seq);
  s->tool = g_strdup(tool);
  s->cwd = g_strdup(cwd);
  s->started = now_ms;
  s->ended = 0;
  s->resume_cmd = g_strdup(resume_cmd ? resume_cmd : "");
  s->note_id = note_id ? g_strdup(note_id) : NULL;
  if (checkpoint_oid != NULL)
    g_strlcpy(s->checkpoint_oid, checkpoint_oid, BT_OID_HEX_LEN);

  g_ptr_array_add(store->items, s);
  if (!save_store(store, error))
    return NULL;
  return s;
}

gboolean bt_sessions_mark_ended(BtSessionStore *store, const gchar *session_id, GError **error)
{
  g_return_val_if_fail(store != NULL && session_id != NULL, FALSE);
  for (guint i = 0; i < store->items->len; i++) {
    BtSession *s = g_ptr_array_index(store->items, i);
    if (strcmp(s->id, session_id) == 0) {
      s->ended = g_get_real_time() / 1000;
      return save_store(store, error);
    }
  }
  return TRUE;
}

const BtSession *bt_sessions_find(const BtSessionStore *store, const gchar *session_id)
{
  if (store == NULL || session_id == NULL)
    return NULL;
  for (guint i = 0; i < store->items->len; i++) {
    const BtSession *s = g_ptr_array_index(store->items, i);
    if (strcmp(s->id, session_id) == 0)
      return s;
  }
  return NULL;
}

const GPtrArray *bt_sessions_list(const BtSessionStore *store)
{
  return store ? store->items : NULL;
}
