/* termtabs.c — Multi-tab terminal GtkNotebook (cap 8, cwd badges, cleanup, §6.D2). */
#include "termtabs.h"

#include <string.h>

typedef struct {
  BtTermTabs *owner;
  BtTerminal *term;
  GtkWidget *page;
  gchar *cwd_abs;
  gchar *badge;
  gchar *session_id;
} TabEntry;

struct BtTermTabs {
  GtkWidget *notebook;
  GPtrArray *entries; /* TabEntry* */
  guint max_tabs;
  BtTerminalFullscreenFn on_fullscreen;
  BtTerminalStatusFn on_status;
  BtTermTabsSessionEndFn on_session_end;
  gpointer user_data;
};

static void tab_entry_free(gpointer p)
{
  TabEntry *e = p;
  if (e == NULL)
    return;
  bt_terminal_free(e->term);
  g_free(e->cwd_abs);
  g_free(e->badge);
  g_free(e->session_id);
  g_free(e);
}

static void on_tab_pty_exit(BtTerminal *term, gint exit_code, gpointer ud)
{
  (void)term;
  (void)exit_code;
  TabEntry *e = ud;
  if (e->session_id != NULL && e->owner->on_session_end != NULL)
    e->owner->on_session_end(e->session_id, e->owner->user_data);
}

static void close_tab_entry(BtTermTabs *tabs, TabEntry *e);

static void on_tab_middle_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                                gpointer ud)
{
  (void)g; (void)x; (void)y;
  if (n_press != 1 || ud == NULL)
    return;
  TabEntry *e = ud;
  close_tab_entry(e->owner, e);
}

static void on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page, guint page_num,
                                    gpointer ud)
{
  (void)nb; (void)page_num;
  BtTermTabs *tabs = ud;
  if (tabs == NULL || page == NULL)
    return;
  for (guint i = 0; i < tabs->entries->len; i++) {
    TabEntry *e = g_ptr_array_index(tabs->entries, i);
    if (e->page == page) {
      bt_terminal_focus(e->term);
      break;
    }
  }
}

static void close_tab_entry(BtTermTabs *tabs, TabEntry *e)
{
  if (e->session_id != NULL && tabs->on_session_end != NULL) {
    tabs->on_session_end(e->session_id, tabs->user_data);
    g_clear_pointer(&e->session_id, g_free);
  }
  gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(tabs->notebook), e->page);
  if (page_num >= 0)
    gtk_notebook_remove_page(GTK_NOTEBOOK(tabs->notebook), page_num);
  g_ptr_array_remove(tabs->entries, e); /* calls tab_entry_free -> bt_terminal_close */
  if (tabs->entries->len > 0)
    bt_termtabs_focus_active(tabs);
}

static void on_close_btn_clicked(GtkButton *btn, gpointer ud)
{
  (void)btn;
  TabEntry *e = ud;
  close_tab_entry(e->owner, e);
}

static void cycle_tabs(BtTermTabs *tabs, gboolean backwards)
{
  if (tabs == NULL || tabs->entries->len < 2)
    return;
  guint count = tabs->entries->len;
  gint current = gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs->notebook));
  if (current < 0)
    return;
  guint step = backwards ? count - 1u : 1u;
  bt_termtabs_select(tabs, ((guint)current + step) % count);
}

static gboolean on_tabs_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                            GdkModifierType state, gpointer ud)
{
  (void)ctrl;
  (void)keycode;
  BtTermTabs *tabs = ud;
  if ((state & GDK_CONTROL_MASK) == 0)
    return FALSE;
  if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
    if (mods != GDK_CONTROL_MASK && mods != (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
      return FALSE;
    cycle_tabs(tabs, keyval == GDK_KEY_ISO_Left_Tab || (mods & GDK_SHIFT_MASK) != 0);
    return TRUE;
  }
  if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_8) {
    bt_termtabs_select(tabs, keyval - GDK_KEY_1);
    return TRUE;
  }
  return FALSE;
}

BtTermTabs *bt_termtabs_new(guint max_tabs, BtTerminalFullscreenFn on_fullscreen,
                            BtTerminalStatusFn on_status, BtTermTabsSessionEndFn on_session_end,
                            gpointer user_data)
{
  BtTermTabs *tabs = g_new0(BtTermTabs, 1);
  tabs->max_tabs = max_tabs > 0 ? max_tabs : BT_TERMTABS_MAX_DEFAULT;
  tabs->on_fullscreen = on_fullscreen;
  tabs->on_status = on_status;
  tabs->on_session_end = on_session_end;
  tabs->user_data = user_data;
  tabs->entries = g_ptr_array_new_with_free_func(tab_entry_free);

  tabs->notebook = gtk_notebook_new();
  gtk_widget_set_name(tabs->notebook, "bt-termtabs");
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(tabs->notebook), TRUE);
  gtk_notebook_set_show_border(GTK_NOTEBOOK(tabs->notebook), FALSE);
  gtk_widget_set_vexpand(tabs->notebook, TRUE);
  gtk_widget_set_hexpand(tabs->notebook, TRUE);
  g_signal_connect(tabs->notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), tabs);

  GtkWidget *plus_btn = gtk_button_new_from_icon_name("bt-plus-symbolic");
  gtk_widget_add_css_class(plus_btn, "bt-iconbtn");
  gtk_widget_set_tooltip_text(plus_btn, "New Terminal Tab");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(plus_btn), "app.term-spawn");
  gtk_notebook_set_action_widget(GTK_NOTEBOOK(tabs->notebook), plus_btn, GTK_PACK_END);

  GtkEventController *key = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_tabs_key), tabs);
  gtk_widget_add_controller(tabs->notebook, key);

  return tabs;
}

void bt_termtabs_close_all(BtTermTabs *tabs)
{
  if (tabs == NULL)
    return;
  while (tabs->entries->len > 0) {
    TabEntry *e = g_ptr_array_index(tabs->entries, tabs->entries->len - 1);
    close_tab_entry(tabs, e);
  }
}

void bt_termtabs_free(BtTermTabs *tabs)
{
  if (tabs == NULL)
    return;
  bt_termtabs_close_all(tabs);
  g_ptr_array_unref(tabs->entries);
  g_free(tabs);
}

GtkWidget *bt_termtabs_widget(BtTermTabs *tabs)
{
  return tabs->notebook;
}

static TabEntry *active_entry(const BtTermTabs *tabs)
{
  if (tabs == NULL || tabs->entries->len == 0)
    return NULL;
  gint idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs->notebook));
  if (idx < 0)
    return g_ptr_array_index(tabs->entries, 0);
  GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(tabs->notebook), idx);
  for (guint i = 0; i < tabs->entries->len; i++) {
    TabEntry *e = g_ptr_array_index(tabs->entries, i);
    if (e->page == page)
      return e;
  }
  return g_ptr_array_index(tabs->entries, 0);
}

gboolean bt_termtabs_open(BtTermTabs *tabs, const BtVault *vault, const gchar *rel_dir,
                          const gchar *active_note_rel, const gchar *session_id, GError **error)
{
  g_return_val_if_fail(tabs != NULL && vault != NULL, FALSE);
  if (tabs->entries->len >= tabs->max_tabs) {
    gchar *msg = g_strdup_printf("Maximum %u terminal tabs reached", tabs->max_tabs);
    if (tabs->on_status != NULL)
      tabs->on_status(msg, tabs->user_data);
    g_set_error_literal(error, BT_ERROR, BT_ERROR_INVALID, msg);
    g_free(msg);
    return FALSE;
  }

  gchar *cwd_abs = NULL;
  if (rel_dir == NULL || rel_dir[0] == '\0' || strcmp(rel_dir, ".") == 0) {
    cwd_abs = g_strdup(vault_root(vault));
  } else {
    cwd_abs = vault_resolve(vault, rel_dir, error);
    if (cwd_abs == NULL)
      return FALSE;
    if (!g_file_test(cwd_abs, G_FILE_TEST_IS_DIR)) {
      gchar *parent = g_path_get_dirname(cwd_abs);
      g_free(cwd_abs);
      cwd_abs = parent;
    }
  }

  gchar *note_env = active_note_rel ? g_strdup_printf("BT_ACTIVE_NOTE=%s", active_note_rel) : NULL;
  const gchar *envp[2] = {note_env, NULL};

  BtTerminal *term = bt_terminal_new(tabs->on_fullscreen, tabs->on_status, tabs->user_data);
  if (!bt_terminal_spawn_with_env(term, cwd_abs, note_env ? envp : NULL, error)) {
    g_free(note_env);
    g_free(cwd_abs);
    bt_terminal_free(term);
    return FALSE;
  }
  g_free(note_env);

  TabEntry *e = g_new0(TabEntry, 1);
  e->owner = tabs;
  e->term = term;
  e->page = bt_terminal_widget(term);
  e->cwd_abs = cwd_abs;
  e->badge = g_path_get_basename(cwd_abs);
  e->session_id = session_id ? g_strdup(session_id) : NULL;
  bt_terminal_set_exit_cb(term, on_tab_pty_exit, e);

  GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *tico = gtk_image_new_from_icon_name("bt-sessions-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(tico), 14);
  GtkWidget *lbl = gtk_label_new(e->badge);
  gtk_widget_set_tooltip_text(tab_box, e->cwd_abs);
  GtkWidget *close_btn = gtk_button_new_with_label("×");
  gtk_widget_add_css_class(close_btn, "flat");
  gtk_widget_set_tooltip_text(close_btn, "Close terminal tab");
  gtk_accessible_update_property(GTK_ACCESSIBLE(close_btn),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Close terminal tab", -1);
  g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_btn_clicked), e);

  GtkGesture *mclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mclick), GDK_BUTTON_MIDDLE);
  g_signal_connect(mclick, "pressed", G_CALLBACK(on_tab_middle_click), e);
  gtk_widget_add_controller(tab_box, GTK_EVENT_CONTROLLER(mclick));
  gtk_box_append(GTK_BOX(tab_box), tico);
  gtk_box_append(GTK_BOX(tab_box), lbl);
  gtk_box_append(GTK_BOX(tab_box), close_btn);

  g_ptr_array_add(tabs->entries, e);
  gint page_idx = gtk_notebook_append_page(GTK_NOTEBOOK(tabs->notebook), e->page, tab_box);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs->notebook), page_idx);
  bt_terminal_focus(term);
  return TRUE;
}

void bt_termtabs_bind_active_session(BtTermTabs *tabs, const gchar *session_id)
{
  TabEntry *e = active_entry(tabs);
  if (e == NULL)
    return;
  g_free(e->session_id);
  e->session_id = session_id ? g_strdup(session_id) : NULL;
}

gboolean bt_termtabs_write_active(BtTermTabs *tabs, const guint8 *data, gsize len)
{
  TabEntry *e = active_entry(tabs);
  if (e == NULL)
    return FALSE;
  return bt_terminal_write(e->term, data, len);
}

void bt_termtabs_focus_active(BtTermTabs *tabs)
{
  TabEntry *e = active_entry(tabs);
  if (e != NULL)
    bt_terminal_focus(e->term);
}

void bt_termtabs_select(BtTermTabs *tabs, guint index)
{
  if (tabs == NULL || index >= tabs->entries->len)
    return;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs->notebook), (gint)index);
  bt_termtabs_focus_active(tabs);
}

void bt_termtabs_next(BtTermTabs *tabs)
{
  cycle_tabs(tabs, FALSE);
}

guint bt_termtabs_count(const BtTermTabs *tabs)
{
  return tabs ? tabs->entries->len : 0;
}

gboolean bt_termtabs_any_running(const BtTermTabs *tabs)
{
  if (tabs == NULL)
    return FALSE;
  for (guint i = 0; i < tabs->entries->len; i++) {
    const TabEntry *e = g_ptr_array_index(tabs->entries, i);
    if (bt_terminal_is_running(e->term))
      return TRUE;
  }
  return FALSE;
}

const gchar *bt_termtabs_active_cwd(const BtTermTabs *tabs)
{
  const TabEntry *e = active_entry(tabs);
  return e ? e->cwd_abs : NULL;
}
