/* editorpane.c — GtkSourceView 5 editor with custom bt-markdown language,
 * soft wrap, 1s debounced autosave, and Ctrl+Click wiki-link navigation (§6.A2). */
#include "editorpane.h"
#include "frontmatter.h"
#include "noteutil.h"

#include <gtksourceview/gtksource.h>
#include <string.h>

#define BT_AUTOSAVE_MS 1000u

struct BtEditorPane {
  GtkWidget *box;
  GtkWidget *conflict_bar;
  GtkWidget *conflict_label;
  GtkWidget *placeholder;
  GtkWidget *scrolled;
  GtkSourceView *view;
  GtkSourceBuffer *buffer;
  gchar *relpath;
  gboolean dirty;
  gboolean loading;
  guint autosave_id;

  BtEditorSaveFn on_save;
  BtEditorLinkFn on_link;
  BtEditorDirtyFn on_dirty;
  BtEditorDiffFn on_diff;
  BtEditorReloadFn on_reload;
  gpointer user_data;
};

static void cancel_autosave(BtEditorPane *pane)
{
  if (pane->autosave_id != 0) {
    g_source_remove(pane->autosave_id);
    pane->autosave_id = 0;
  }
}

static void emit_save(BtEditorPane *pane)
{
  if (pane->relpath == NULL || pane->on_save == NULL)
    return;
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &start, &end);
  gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &start, &end, TRUE);
  pane->on_save(pane->relpath, text, strlen(text), pane->user_data);
  g_free(text);
}

static gboolean on_autosave_timeout(gpointer ud)
{
  BtEditorPane *pane = ud;
  pane->autosave_id = 0;
  if (pane->dirty && pane->relpath != NULL)
    emit_save(pane);
  return G_SOURCE_REMOVE;
}

static void on_buffer_changed(GtkTextBuffer *buf, gpointer ud)
{
  (void)buf;
  BtEditorPane *pane = ud;
  if (pane->loading || pane->relpath == NULL)
    return;
  if (!pane->dirty) {
    pane->dirty = TRUE;
    if (pane->on_dirty != NULL)
      pane->on_dirty(TRUE, pane->user_data);
  }
  cancel_autosave(pane);
  pane->autosave_id = g_timeout_add(BT_AUTOSAVE_MS, on_autosave_timeout, pane);
}

static void on_editor_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                            gpointer ud)
{
  BtEditorPane *pane = ud;
  if (n_press != 1 || pane->on_link == NULL || pane->relpath == NULL)
    return;
  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
  GdkEvent *ev = gtk_gesture_get_last_event(GTK_GESTURE(gesture), seq);
  if (ev == NULL)
    return;
  GdkModifierType state = gdk_event_get_modifier_state(ev);
  if ((state & GDK_CONTROL_MASK) == 0)
    return;

  gint bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(pane->view), GTK_TEXT_WINDOW_WIDGET,
                                        (gint)x, (gint)y, &bx, &by);
  GtkTextIter iter;
  if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(pane->view), &iter, bx, by))
    return;

  GtkTextIter line_start = iter, line_end = iter;
  gtk_text_iter_set_line_offset(&line_start, 0);
  if (!gtk_text_iter_ends_line(&line_end))
    gtk_text_iter_forward_to_line_end(&line_end);

  gchar *line = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &line_start, &line_end,
                                         FALSE);
  gint line_byte_off = gtk_text_iter_get_line_index(&iter);
  gchar *target = bt_wikilink_at(line, (gsize)MAX(line_byte_off, 0));
  g_free(line);
  if (target != NULL) {
    pane->on_link(target, pane->user_data);
    g_free(target);
  }
}

static gchar *find_exe_dir(void)
{
#ifdef G_OS_WIN32
  return g_win32_get_package_installation_directory_of_module(NULL);
#else
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe == NULL)
    return NULL;
  gchar *dir = g_path_get_dirname(exe);
  g_free(exe);
  return dir;
#endif
}

static gboolean path_array_has(const gchar *const *arr, const gchar *path)
{
  for (guint j = 0; arr != NULL && arr[j] != NULL; j++) {
    if (strcmp(arr[j], path) == 0)
      return TRUE;
  }
  return FALSE;
}

static void add_dir_candidate(GPtrArray *out, const gchar *path, gboolean require_dir)
{
  if (path == NULL || path[0] == '\0')
    return;
  if (require_dir && !g_file_test(path, G_FILE_TEST_IS_DIR))
    return;
  g_ptr_array_add(out, g_strdup(path));
}

static GPtrArray *collect_subdir_paths(const gchar *compile_dir, const gchar *subdir,
                                       const gchar *res_uri)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (res_uri != NULL)
    add_dir_candidate(out, res_uri, FALSE);
  add_dir_candidate(out, compile_dir, TRUE);

  gchar *cwd = g_get_current_dir();
  gchar *cwd_dir = g_build_filename(cwd, "data", subdir, NULL);
  add_dir_candidate(out, cwd_dir, TRUE);
  g_free(cwd_dir);
  g_free(cwd);

  gchar *exe_dir = find_exe_dir();
  if (exe_dir != NULL) {
    gchar *pkg1 = g_build_filename(exe_dir, "share", "brainterm", subdir, NULL);
    gchar *pkg2 = g_build_filename(exe_dir, "..", "share", "brainterm", subdir, NULL);
    gchar *dev1 = g_build_filename(exe_dir, "..", "..", "data", subdir, NULL);
    add_dir_candidate(out, pkg1, TRUE);
    add_dir_candidate(out, pkg2, TRUE);
    add_dir_candidate(out, dev1, TRUE);
    g_free(pkg1);
    g_free(pkg2);
    g_free(dev1);
    g_free(exe_dir);
  }

  const gchar *const *sys_dirs = g_get_system_data_dirs();
  for (guint i = 0; sys_dirs != NULL && sys_dirs[i] != NULL; i++) {
    gchar *p = g_build_filename(sys_dirs[i], "brainterm", subdir, NULL);
    add_dir_candidate(out, p, TRUE);
    g_free(p);
  }
  return out;
}

static void configure_language(GtkSourceBuffer *buffer)
{
  GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
  const gchar *compile_dir = NULL;
#ifdef BT_LANGSPEC_DIR
  compile_dir = BT_LANGSPEC_DIR;
#endif
  GPtrArray *paths = collect_subdir_paths(compile_dir, "language-specs", NULL);
  const gchar *const *cur = gtk_source_language_manager_get_search_path(lm);
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = g_ptr_array_index(paths, i);
    if (!path_array_has(cur, p)) {
      gtk_source_language_manager_append_search_path(lm, p);
      cur = gtk_source_language_manager_get_search_path(lm);
    }
  }
  g_ptr_array_unref(paths);

  GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, "bt-markdown");
  if (lang == NULL)
    lang = gtk_source_language_manager_get_language(lm, "markdown");
  if (lang != NULL) {
    gtk_source_buffer_set_language(buffer, lang);
    gtk_source_buffer_set_highlight_syntax(buffer, TRUE);
  }
}

static void apply_ink_scheme(GtkSourceBuffer *buffer, gboolean dark)
{
  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  const gchar *compile_dir = NULL;
#ifdef BT_STYLES_DIR
  compile_dir = BT_STYLES_DIR;
#endif
  GPtrArray *paths = collect_subdir_paths(compile_dir, "styles", "resource:///local/brainterm/styles");
  const gchar *const *cur = gtk_source_style_scheme_manager_get_search_path(sm);
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = g_ptr_array_index(paths, i);
    if (!path_array_has(cur, p)) {
      gtk_source_style_scheme_manager_append_search_path(sm, p);
      cur = gtk_source_style_scheme_manager_get_search_path(sm);
    }
  }
  g_ptr_array_unref(paths);

  GtkSourceStyleScheme *sc =
      gtk_source_style_scheme_manager_get_scheme(sm, dark ? "bt-ink-dark" : "bt-ink-light");
  if (sc != NULL)
    gtk_source_buffer_set_style_scheme(buffer, sc);
}

static void on_conflict_diff(GtkButton *b, gpointer ud)
{
  (void)b;
  BtEditorPane *p = ud;
  if (p->on_diff != NULL && p->relpath != NULL)
    p->on_diff(p->relpath, p->user_data);
}

static void on_conflict_keep(GtkButton *b, gpointer ud)
{
  (void)b;
  BtEditorPane *p = ud;
  gtk_widget_set_visible(p->conflict_bar, FALSE);
  bt_editorpane_flush_save(p);
}

static void on_conflict_reload(GtkButton *b, gpointer ud)
{
  (void)b;
  BtEditorPane *p = ud;
  gtk_widget_set_visible(p->conflict_bar, FALSE);
  if (p->on_reload != NULL && p->relpath != NULL)
    p->on_reload(p->relpath, p->user_data);
}

BtEditorPane *bt_editorpane_new(BtEditorSaveFn on_save, BtEditorLinkFn on_link,
                                BtEditorDirtyFn on_dirty, gpointer user_data)
{
  BtEditorPane *pane = g_new0(BtEditorPane, 1);
  pane->on_save = on_save;
  pane->on_link = on_link;
  pane->on_dirty = on_dirty;
  pane->user_data = user_data;

  pane->buffer = gtk_source_buffer_new(NULL);
  configure_language(pane->buffer);
  apply_ink_scheme(pane->buffer, TRUE);
  g_signal_connect(pane->buffer, "changed", G_CALLBACK(on_buffer_changed), pane);

  pane->view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(pane->buffer));
  gtk_widget_set_name(GTK_WIDGET(pane->view), "bt-editor-view");
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(pane->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(pane->view), 8);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(pane->view), 8);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(pane->view), 12);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(pane->view), 12);
  gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(pane->view), 3);
  gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(pane->view), 3);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(pane->view), TRUE);
  gtk_source_view_set_show_line_numbers(pane->view, TRUE);
  gtk_source_view_set_highlight_current_line(pane->view, TRUE);
  gtk_source_view_set_tab_width(pane->view, 2);
  gtk_source_view_set_insert_spaces_instead_of_tabs(pane->view, TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(pane->view), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(pane->view), TRUE);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_editor_click), pane);
  gtk_widget_add_controller(GTK_WIDGET(pane->view), GTK_EVENT_CONTROLLER(click));

  pane->scrolled = gtk_scrolled_window_new();
  gtk_widget_set_name(pane->scrolled, "bt-editor-scrolled");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pane->scrolled), GTK_WIDGET(pane->view));
  gtk_widget_set_vexpand(pane->scrolled, TRUE);
  gtk_widget_set_hexpand(pane->scrolled, TRUE);

  pane->placeholder = gtk_label_new("Open a vault or select a Markdown note (Ctrl+P)");
  gtk_widget_set_name(pane->placeholder, "bt-editor-placeholder");
  gtk_widget_add_css_class(pane->placeholder, "dim-label");
  gtk_widget_set_vexpand(pane->placeholder, TRUE);
  gtk_widget_set_hexpand(pane->placeholder, TRUE);

  /* Conflict banner (§6.A5): never auto-overwrites either side. */
  pane->conflict_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(pane->conflict_bar, "bt-banner");
  gtk_widget_set_margin_start(pane->conflict_bar, 10);
  gtk_widget_set_margin_end(pane->conflict_bar, 10);
  gtk_widget_set_margin_top(pane->conflict_bar, 6);
  gtk_widget_set_margin_bottom(pane->conflict_bar, 6);
  pane->conflict_label = gtk_label_new("File changed on disk (an agent may have edited it)");
  gtk_label_set_xalign(GTK_LABEL(pane->conflict_label), 0.0f);
  gtk_widget_set_hexpand(pane->conflict_label, TRUE);
  GtkWidget *b_diff = gtk_button_new_with_label("Review diff");
  GtkWidget *b_keep = gtk_button_new_with_label("Keep mine");
  GtkWidget *b_rel = gtk_button_new_with_label("Reload");
  g_signal_connect(b_diff, "clicked", G_CALLBACK(on_conflict_diff), pane);
  g_signal_connect(b_keep, "clicked", G_CALLBACK(on_conflict_keep), pane);
  g_signal_connect(b_rel, "clicked", G_CALLBACK(on_conflict_reload), pane);
  gtk_box_append(GTK_BOX(pane->conflict_bar), pane->conflict_label);
  gtk_box_append(GTK_BOX(pane->conflict_bar), b_diff);
  gtk_box_append(GTK_BOX(pane->conflict_bar), b_keep);
  gtk_box_append(GTK_BOX(pane->conflict_bar), b_rel);
  gtk_widget_set_visible(pane->conflict_bar, FALSE);

  pane->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(pane->box, "bt-editor-box");
  gtk_widget_set_hexpand(pane->box, TRUE);
  gtk_widget_set_vexpand(pane->box, TRUE);
  gtk_box_append(GTK_BOX(pane->box), pane->conflict_bar);
  gtk_box_append(GTK_BOX(pane->box), pane->placeholder);
  gtk_box_append(GTK_BOX(pane->box), pane->scrolled);
  gtk_widget_set_visible(pane->scrolled, FALSE);

  return pane;
}

void bt_editorpane_set_conflict_cbs(BtEditorPane *pane, BtEditorDiffFn on_diff,
                                    BtEditorReloadFn on_reload)
{
  if (pane == NULL)
    return;
  pane->on_diff = on_diff;
  pane->on_reload = on_reload;
}

void bt_editorpane_show_conflict(BtEditorPane *pane, gboolean deleted)
{
  if (pane == NULL)
    return;
  gtk_label_set_text(GTK_LABEL(pane->conflict_label),
                     deleted ? "File was deleted on disk — [Keep mine] to recreate or [Reload] to close"
                             : "File changed on disk (an agent may have edited it)");
  gtk_widget_set_visible(pane->conflict_bar, TRUE);
}

void bt_editorpane_hide_conflict(BtEditorPane *pane)
{
  if (pane != NULL)
    gtk_widget_set_visible(pane->conflict_bar, FALSE);
}

void bt_editorpane_set_dark(BtEditorPane *pane, gboolean dark)
{
  if (pane != NULL && pane->buffer != NULL)
    apply_ink_scheme(pane->buffer, dark);
}

gchar *bt_editorpane_buffer_hash(const BtEditorPane *pane)
{
  if (pane == NULL)
    return NULL;
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &s, &e);
  gchar *txt = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &s, &e, TRUE);
  gchar *h = bt_fnv1a_hex(txt, strlen(txt));
  g_free(txt);
  return h;
}

void bt_editorpane_free(BtEditorPane *pane)
{
  if (pane == NULL)
    return;
  cancel_autosave(pane);
  g_free(pane->relpath);
  g_clear_object(&pane->buffer);
  g_free(pane);
}

GtkWidget *bt_editorpane_widget(BtEditorPane *pane)
{
  return pane->box;
}

GtkWidget *bt_editorpane_view_widget(BtEditorPane *pane)
{
  return pane ? GTK_WIDGET(pane->view) : NULL;
}

void bt_editorpane_load(BtEditorPane *pane, const gchar *relpath, const gchar *text, gsize length)
{
  g_return_if_fail(pane != NULL && relpath != NULL);
  cancel_autosave(pane);
  g_free(pane->relpath);
  pane->relpath = g_strdup(relpath);

  pane->loading = TRUE;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pane->buffer), text ? text : "", (gint)length);
  GtkTextIter start;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(pane->buffer), &start);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(pane->buffer), &start);
  pane->loading = FALSE;

  pane->dirty = FALSE;
  if (pane->on_dirty != NULL)
    pane->on_dirty(FALSE, pane->user_data);
  gtk_widget_set_visible(pane->placeholder, FALSE);
  gtk_widget_set_visible(pane->scrolled, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(pane->view));
}

void bt_editorpane_clear(BtEditorPane *pane)
{
  g_return_if_fail(pane != NULL);
  cancel_autosave(pane);
  g_clear_pointer(&pane->relpath, g_free);
  pane->loading = TRUE;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pane->buffer), "", 0);
  pane->loading = FALSE;
  pane->dirty = FALSE;
  gtk_widget_set_visible(pane->scrolled, FALSE);
  gtk_widget_set_visible(pane->placeholder, TRUE);
}

const gchar *bt_editorpane_relpath(const BtEditorPane *pane)
{
  return pane ? pane->relpath : NULL;
}

gboolean bt_editorpane_is_dirty(const BtEditorPane *pane)
{
  return pane ? pane->dirty : FALSE;
}

const gchar *bt_editorpane_scheme_id(const BtEditorPane *pane)
{
  if (pane == NULL || pane->buffer == NULL)
    return "none";
  GtkSourceStyleScheme *sc = gtk_source_buffer_get_style_scheme(pane->buffer);
  return sc ? gtk_source_style_scheme_get_id(sc) : "none";
}

void bt_editorpane_flush_save(BtEditorPane *pane)
{
  if (pane == NULL || pane->relpath == NULL)
    return;
  cancel_autosave(pane);
  emit_save(pane);
}

void bt_editorpane_mark_saved(BtEditorPane *pane)
{
  if (pane == NULL)
    return;
  pane->dirty = FALSE;
  if (pane->on_dirty != NULL)
    pane->on_dirty(FALSE, pane->user_data);
}
