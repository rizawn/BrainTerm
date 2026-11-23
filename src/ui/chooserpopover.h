/* chooserpopover.h — Ambiguity chooser popover widget for multi-match wikilinks (L3-2b). */
#ifndef BT_CHOOSERPOPOVER_H
#define BT_CHOOSERPOPOVER_H

#include <gtk/gtk.h>

#include "choosermodel.h"

G_BEGIN_DECLS

typedef struct BtChooserPopover BtChooserPopover;

typedef void (*BtChooserOpenFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtChooserCopyFn)(const gchar *link_text, const gchar *status_msg, gpointer user_data);
typedef void (*BtChooserCancelFn)(gpointer user_data);

BtChooserPopover *bt_chooserpopover_new(BtChooserOpenFn on_open, BtChooserCopyFn on_copy,
                                        BtChooserCancelFn on_cancel, gpointer user_data);
void bt_chooserpopover_free(BtChooserPopover *cp);

void bt_chooserpopover_show(BtChooserPopover *cp, GtkWidget *anchor_widget,
                            const GdkRectangle *rect, const gchar *raw_target,
                            const gchar *const *candidates);
void bt_chooserpopover_close(BtChooserPopover *cp);

gboolean bt_chooserpopover_is_open(const BtChooserPopover *cp);
guint bt_chooserpopover_selected_index(const BtChooserPopover *cp);
guint bt_chooserpopover_candidate_count(const BtChooserPopover *cp);
const gchar *bt_chooserpopover_title_text(const BtChooserPopover *cp);
const gchar *bt_chooserpopover_footer_text(const BtChooserPopover *cp);

gboolean bt_chooserpopover_handle_key(BtChooserPopover *cp, guint keyval, GdkModifierType state);
void bt_chooserpopover_activate_open(BtChooserPopover *cp);
void bt_chooserpopover_activate_copy(BtChooserPopover *cp);
void bt_chooserpopover_activate_cancel(BtChooserPopover *cp);

G_END_DECLS

#endif
