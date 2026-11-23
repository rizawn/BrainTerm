/* choosermodel.c — UI-free model for ambiguous wikilink selection (Phase L3-2a). */
#include "choosermodel.h"

#include <string.h>

struct BtChooserModel {
  gchar *target;
  BtChooserItem *items;
  guint count;
  guint selected;
};

static gchar *strip_markdown_ext(const gchar *text)
{
  if (text == NULL)
    return g_strdup("");
  gsize len = strlen(text);
  if (len > 9 && g_ascii_strcasecmp(text + len - 9, ".markdown") == 0)
    return g_strndup(text, len - 9);
  if (len > 3 && g_ascii_strcasecmp(text + len - 3, ".md") == 0)
    return g_strndup(text, len - 3);
  return g_strdup(text);
}

static void split_relpath(const gchar *relpath, gchar **out_folder, gchar **out_filename)
{
  const gchar *clean = relpath ? relpath : "";
  const gchar *slash = strrchr(clean, '/');
  if (slash == NULL) {
    *out_folder = g_strdup("");
    *out_filename = g_strdup(clean);
  } else {
    *out_folder = g_strndup(clean, (gsize)(slash - clean));
    *out_filename = g_strdup(slash + 1);
  }
}

BtChooserModel *bt_chooser_model_new(const gchar *raw_target, const gchar *const *candidates)
{
  BtChooserModel *m = g_new0(BtChooserModel, 1);
  m->target = g_strdup(raw_target ? raw_target : "");

  guint n = 0;
  if (candidates != NULL) {
    while (candidates[n] != NULL)
      n++;
  }
  m->count = n;
  m->selected = 0u;
  if (n > 0) {
    m->items = g_new0(BtChooserItem, n);
    for (guint i = 0; i < n; i++) {
      m->items[i].relpath = g_strdup(candidates[i]);
      split_relpath(candidates[i], &m->items[i].folder, &m->items[i].filename);
    }
  }
  return m;
}

void bt_chooser_model_free(BtChooserModel *m)
{
  if (m == NULL)
    return;
  for (guint i = 0; i < m->count; i++) {
    g_free(m->items[i].relpath);
    g_free(m->items[i].filename);
    g_free(m->items[i].folder);
  }
  g_free(m->items);
  g_free(m->target);
  g_free(m);
}

guint bt_chooser_model_count(const BtChooserModel *m)
{
  return m ? m->count : 0u;
}

guint bt_chooser_model_selected(const BtChooserModel *m)
{
  return m ? m->selected : 0u;
}

gboolean bt_chooser_model_select(BtChooserModel *m, guint index)
{
  if (m == NULL || index >= m->count)
    return FALSE;
  m->selected = index;
  return TRUE;
}

guint bt_chooser_model_move_down(BtChooserModel *m)
{
  if (m == NULL || m->count == 0)
    return 0u;
  m->selected = (m->selected + 1u) % m->count;
  return m->selected;
}

guint bt_chooser_model_move_up(BtChooserModel *m)
{
  if (m == NULL || m->count == 0)
    return 0u;
  m->selected = (m->selected == 0u) ? (m->count - 1u) : (m->selected - 1u);
  return m->selected;
}

guint bt_chooser_model_home(BtChooserModel *m)
{
  if (m == NULL)
    return 0u;
  m->selected = 0u;
  return m->selected;
}

guint bt_chooser_model_end(BtChooserModel *m)
{
  if (m == NULL || m->count == 0)
    return 0u;
  m->selected = m->count - 1u;
  return m->selected;
}

guint bt_chooser_model_visible_count(const BtChooserModel *m)
{
  if (m == NULL)
    return 0u;
  return (m->count < BT_CHOOSER_MAX_VISIBLE) ? m->count : BT_CHOOSER_MAX_VISIBLE;
}

guint bt_chooser_model_overflow_count(const BtChooserModel *m)
{
  if (m == NULL || m->count <= BT_CHOOSER_MAX_VISIBLE)
    return 0u;
  return m->count - BT_CHOOSER_MAX_VISIBLE;
}

gchar *bt_chooser_model_title(const BtChooserModel *m)
{
  guint cnt = m ? m->count : 0u;
  const gchar *tgt = (m && m->target) ? m->target : "";
  return g_strdup_printf("%u notes match \"%s\"", cnt, tgt);
}

gchar *bt_chooser_model_footer_more(const BtChooserModel *m)
{
  guint over = bt_chooser_model_overflow_count(m);
  if (over == 0u)
    return NULL;
  return g_strdup_printf("+%u more", over);
}

const BtChooserItem *bt_chooser_model_item(const BtChooserModel *m, guint index)
{
  if (m == NULL || index >= m->count)
    return NULL;
  return &m->items[index];
}

const gchar *bt_chooser_model_selected_relpath(const BtChooserModel *m)
{
  if (m == NULL || m->selected >= m->count)
    return NULL;
  return m->items[m->selected].relpath;
}

gchar *bt_chooser_model_copy_link(const BtChooserModel *m, guint index)
{
  if (m == NULL || index >= m->count)
    return NULL;
  g_autofree gchar *stripped = strip_markdown_ext(m->items[index].relpath);
  return g_strdup_printf("[[%s]]", stripped);
}

gchar *bt_chooser_model_copy_status(const BtChooserModel *m, guint index)
{
  if (m == NULL || index >= m->count)
    return NULL;
  g_autofree gchar *name = strip_markdown_ext(m->items[index].filename);
  return g_strdup_printf("Copied link for %s", name);
}

BtChooserOpenAction bt_chooser_decide_open(const gchar *const *candidates)
{
  guint n = 0;
  if (candidates != NULL) {
    while (candidates[n] != NULL)
      n++;
  }
  if (n == 0u)
    return BT_CHOOSER_OPEN_NONE_MATCH;
  if (n == 1u)
    return BT_CHOOSER_OPEN_DIRECT;
  return BT_CHOOSER_OPEN_SHOW;
}

gboolean bt_chooser_candidate_exists(const BtVault *vault, const gchar *relpath,
                                     gchar **out_status_msg)
{
  if (out_status_msg != NULL)
    *out_status_msg = NULL;
  if (vault == NULL || relpath == NULL || relpath[0] == '\0') {
    if (out_status_msg != NULL)
      *out_status_msg = g_strdup("That note no longer exists");
    return FALSE;
  }
  g_autofree gchar *abs_path = vault_resolve(vault, relpath, NULL);
  if (abs_path == NULL || !g_file_test(abs_path, G_FILE_TEST_IS_REGULAR)) {
    if (out_status_msg != NULL)
      *out_status_msg = g_strdup("That note no longer exists");
    return FALSE;
  }
  return TRUE;
}
