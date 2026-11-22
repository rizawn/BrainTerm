/* navhistory.c — In-memory back/forward navigation stack (L2-6). */
#include "navhistory.h"

#include <string.h>

struct BtNavHistory {
  GArray *entries; /* of BtNavEntry */
  gint cursor;     /* -1 when empty */
};

static void clear_nav_entry(gpointer data)
{
  BtNavEntry *e = data;
  g_free(e->relpath);
  e->relpath = NULL;
}

BtNavHistory *bt_nav_history_new(void)
{
  BtNavHistory *h = g_new0(BtNavHistory, 1);
  h->entries = g_array_new(FALSE, TRUE, sizeof(BtNavEntry));
  g_array_set_clear_func(h->entries, clear_nav_entry);
  h->cursor = -1;
  return h;
}

void bt_nav_history_free(BtNavHistory *history)
{
  if (history == NULL)
    return;
  if (history->entries != NULL)
    g_array_free(history->entries, TRUE);
  g_free(history);
}

void bt_nav_history_clear(BtNavHistory *history)
{
  if (history == NULL)
    return;
  g_array_set_size(history->entries, 0);
  history->cursor = -1;
}

static void append_capped(BtNavHistory *h, const gchar *relpath, gsize caret, gdouble scroll_y)
{
  BtNavEntry e = {
      .relpath = g_strdup(relpath),
      .caret_offset = caret,
      .scroll_y = scroll_y,
  };
  g_array_append_val(h->entries, e);
  while (h->entries->len > BT_NAV_HISTORY_CAP) {
    g_array_remove_index(h->entries, 0);
  }
  h->cursor = (gint)h->entries->len - 1;
}

void bt_nav_history_push_jump(BtNavHistory *history, const gchar *from_relpath, gsize from_caret,
                              gdouble from_scroll_y, const gchar *to_relpath, gsize to_caret,
                              gdouble to_scroll_y)
{
  if (history == NULL || to_relpath == NULL || to_relpath[0] == '\0')
    return;

  if (history->entries->len > 0 && history->cursor >= 0 &&
      (guint)(history->cursor + 1) < history->entries->len) {
    g_array_set_size(history->entries, (guint)(history->cursor + 1));
  }

  if (from_relpath != NULL && from_relpath[0] != '\0') {
    if (history->cursor >= 0 && (guint)history->cursor < history->entries->len) {
      BtNavEntry *cur = &g_array_index(history->entries, BtNavEntry, (guint)history->cursor);
      if (g_strcmp0(cur->relpath, from_relpath) == 0) {
        cur->caret_offset = from_caret;
        cur->scroll_y = from_scroll_y;
      } else {
        append_capped(history, from_relpath, from_caret, from_scroll_y);
      }
    } else {
      append_capped(history, from_relpath, from_caret, from_scroll_y);
    }
  } else if (history->entries->len == 1 && history->cursor == 0) {
    BtNavEntry *only = &g_array_index(history->entries, BtNavEntry, 0);
    if (g_strcmp0(only->relpath, to_relpath) == 0) {
      only->caret_offset = to_caret;
      only->scroll_y = to_scroll_y;
      return;
    }
  }

  append_capped(history, to_relpath, to_caret, to_scroll_y);
}

void bt_nav_history_update_current(BtNavHistory *history, const gchar *relpath, gsize caret_offset,
                                   gdouble scroll_y)
{
  if (history == NULL || relpath == NULL || history->cursor < 0 ||
      (guint)history->cursor >= history->entries->len)
    return;
  BtNavEntry *cur = &g_array_index(history->entries, BtNavEntry, (guint)history->cursor);
  if (g_strcmp0(cur->relpath, relpath) == 0) {
    cur->caret_offset = caret_offset;
    cur->scroll_y = scroll_y;
  }
}

gsize bt_nav_history_count(const BtNavHistory *history)
{
  return history ? (gsize)history->entries->len : 0;
}

gint bt_nav_history_cursor(const BtNavHistory *history)
{
  return history ? history->cursor : -1;
}

const BtNavEntry *bt_nav_history_at(const BtNavHistory *history, gsize index)
{
  if (history == NULL || index >= history->entries->len)
    return NULL;
  return &g_array_index(history->entries, BtNavEntry, index);
}

gboolean bt_nav_history_can_go_back(const BtNavHistory *history)
{
  return history != NULL && history->cursor > 0 && history->entries->len > 1;
}

gboolean bt_nav_history_can_go_forward(const BtNavHistory *history)
{
  return history != NULL && history->cursor >= 0 &&
         (guint)(history->cursor + 1) < history->entries->len;
}

void bt_nav_history_set_cursor(BtNavHistory *history, gint cursor)
{
  if (history == NULL || history->entries->len == 0)
    return;
  if (cursor < 0)
    cursor = 0;
  if ((guint)cursor >= history->entries->len)
    cursor = (gint)history->entries->len - 1;
  history->cursor = cursor;
}

void bt_nav_history_remove_at(BtNavHistory *history, gsize index)
{
  if (history == NULL || index >= history->entries->len)
    return;
  g_array_remove_index(history->entries, (guint)index);
  if (history->entries->len == 0) {
    history->cursor = -1;
  } else if ((gint)index < history->cursor) {
    history->cursor--;
  } else if ((guint)history->cursor >= history->entries->len) {
    history->cursor = (gint)history->entries->len - 1;
  }
}
