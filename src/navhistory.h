/* navhistory.h — In-memory back/forward navigation stack (L2-6 / ADR-016). */
#ifndef BT_NAVHISTORY_H
#define BT_NAVHISTORY_H

#include <glib.h>

G_BEGIN_DECLS

#define BT_NAV_HISTORY_CAP 100

typedef struct {
  gchar *relpath;
  gsize caret_offset;
  gdouble scroll_y;
} BtNavEntry;

typedef struct BtNavHistory BtNavHistory;

BtNavHistory *bt_nav_history_new(void);
void bt_nav_history_free(BtNavHistory *history);
void bt_nav_history_clear(BtNavHistory *history);

/* Records transition from the current note position to a new destination.
 * Updates current entry (or initializes it if empty), drops any forward tail,
 * appends the new destination, and caps total history at BT_NAV_HISTORY_CAP. */
void bt_nav_history_push_jump(BtNavHistory *history, const gchar *from_relpath, gsize from_caret,
                              gdouble from_scroll_y, const gchar *to_relpath, gsize to_caret,
                              gdouble to_scroll_y);

/* Updates caret and scroll position of the active entry when relpath matches. */
void bt_nav_history_update_current(BtNavHistory *history, const gchar *relpath, gsize caret_offset,
                                   gdouble scroll_y);

gsize bt_nav_history_count(const BtNavHistory *history);
gint bt_nav_history_cursor(const BtNavHistory *history);
const BtNavEntry *bt_nav_history_at(const BtNavHistory *history, gsize index);
gboolean bt_nav_history_can_go_back(const BtNavHistory *history);
gboolean bt_nav_history_can_go_forward(const BtNavHistory *history);
void bt_nav_history_set_cursor(BtNavHistory *history, gint cursor);
void bt_nav_history_remove_at(BtNavHistory *history, gsize index);

G_END_DECLS

#endif
