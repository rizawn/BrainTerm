/* createpopover.h — Create-note confirmation popover UI (Phase L3-1d, §6.1, §14.1). */
#ifndef BT_CREATEPOPOVER_H
#define BT_CREATEPOPOVER_H

#include <gtk/gtk.h>

#include "createnote.h"

G_BEGIN_DECLS

typedef enum {
  BT_CREATE_POPOVER_CLOSED = 0,
  BT_CREATE_POPOVER_SHOWN = 1,
  BT_CREATE_POPOVER_BUSY = 2
} BtCreatePopoverState;

typedef void (*BtCreateConfirmFn)(const BtCreatePlan *plan, gpointer user_data);
typedef void (*BtCreateCancelFn)(gpointer user_data);

typedef struct BtCreatePopover BtCreatePopover;

BtCreatePopover *bt_createpopover_new(BtCreateConfirmFn on_confirm, BtCreateCancelFn on_cancel,
                                      gpointer user_data);
void bt_createpopover_free(BtCreatePopover *cp);

/* Shows popover anchored to parent_widget at pointing_rect iff plan->result == BT_CREATE_PLAN_OK.
 * Returns FALSE and remains CLOSED for any non-OK plan result. */
gboolean bt_createpopover_show(BtCreatePopover *cp, GtkWidget *parent_widget,
                               const GdkRectangle *pointing_rect, const BtCreatePlan *plan);

void bt_createpopover_set_busy(BtCreatePopover *cp, gboolean busy);
void bt_createpopover_close(BtCreatePopover *cp);

BtCreatePopoverState bt_createpopover_state(const BtCreatePopover *cp);
const BtCreatePlan *bt_createpopover_plan(const BtCreatePopover *cp);
GtkPopover *bt_createpopover_widget(const BtCreatePopover *cp);
GtkWidget *bt_createpopover_create_button(const BtCreatePopover *cp);
GtkWidget *bt_createpopover_cancel_button(const BtCreatePopover *cp);
GtkWidget *bt_createpopover_focused_button(const BtCreatePopover *cp);
const gchar *bt_createpopover_title_text(const BtCreatePopover *cp);
const gchar *bt_createpopover_path_text(const BtCreatePopover *cp);
const gchar *bt_createpopover_location_hint_text(const BtCreatePopover *cp);
const gchar *bt_createpopover_warning_text(const BtCreatePopover *cp);
const gchar *bt_createpopover_create_button_label(const BtCreatePopover *cp);

/* Keyboard / programmatic action triggers (for tests & Enter/Esc handling). */
void bt_createpopover_activate_create(BtCreatePopover *cp);
void bt_createpopover_activate_cancel(BtCreatePopover *cp);
gboolean bt_createpopover_handle_key(BtCreatePopover *cp, guint keyval, GdkModifierType state);

G_END_DECLS

#endif
