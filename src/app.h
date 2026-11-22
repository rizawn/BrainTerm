/* app.h — BtApp: application state, vault session, state.ini, action routing (§5.2, §5.5). */
#ifndef BT_APP_H
#define BT_APP_H

#include <gtk/gtk.h>

#include "linkresolve.h"
#include "navhistory.h"
#include "vault.h"

G_BEGIN_DECLS

typedef struct BtApp BtApp;

/* Creates the GtkApplication and registers all M1 GActions. */
BtApp *bt_app_new(void);
void bt_app_free(BtApp *app);
int bt_app_run(BtApp *app, int argc, char **argv);
const gchar *bt_app_build_stamp(void);

/* State persistence (§6.A1, §7.5): lives in user config dir, never in vault. */
gchar *bt_app_state_file_path(void);
gchar *bt_app_load_last_vault(void);
gboolean bt_app_save_last_vault(const gchar *vault_path, GError **error);
const gchar *bt_app_resolve_page_name(const gchar *saved);
gchar *bt_app_load_saved_page(void);
void bt_app_save_page(const gchar *page);
gboolean bt_app_load_links_open_on_click(void);
gboolean bt_app_save_links_open_on_click(gboolean enabled, GError **error);
void bt_app_set_links_open_on_click(BtApp *app, gboolean enabled, gboolean persist);
gboolean bt_app_get_links_open_on_click(const BtApp *app);
gpointer bt_app_application(const BtApp *app);

/* Accessors used by UI panes. */
BtVault *bt_app_vault(const BtApp *app);
GtkWindow *bt_app_window(const BtApp *app);

/* Vault & note actions (all funnel through GActions where user-triggered). */
gboolean bt_app_open_vault(BtApp *app, const gchar *path, GError **error);
gboolean bt_app_open_note(BtApp *app, const gchar *relpath, GError **error);
gboolean bt_app_activate_link(BtApp *app, const gchar *raw_target);
gboolean bt_app_open_or_create_wikilink(BtApp *app, const gchar *target, GError **error);
gboolean bt_app_save_active(BtApp *app, GError **error);
gboolean bt_app_navigate_back(BtApp *app);
gboolean bt_app_navigate_forward(BtApp *app);
BtNavHistory *bt_app_nav_history(const BtApp *app);

void bt_app_show_error(BtApp *app, const gchar *title, const gchar *detail);
void bt_app_set_status(BtApp *app, const gchar *message);
void bt_app_set_statusbar_notice(BtApp *app, const gchar *message);
const gchar *bt_app_last_statusbar_notice(const BtApp *app);
void bt_app_set_fail_next_save_for_test(BtApp *app, gboolean fail);
void bt_app_set_launcher_for_test(BtApp *app, BtLinkLauncherFn launcher, gpointer user_data);
GtkPopover *bt_app_active_confirm_popover_for_test(const BtApp *app);
const gchar *bt_app_active_confirm_detail_for_test(const BtApp *app);
gboolean bt_app_confirm_active_popover_for_test(BtApp *app, gboolean dont_ask_again);
const gchar *bt_app_active_note(const BtApp *app);
gpointer bt_app_editor(const BtApp *app);
void bt_app_set_test_mode(BtApp *app, gboolean enabled);
void bt_app_prepare_ui_for_test(BtApp *app);

G_END_DECLS

#endif
