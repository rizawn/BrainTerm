/* themeload.c — theme.css loading, hot reload, icon + font registration. */
#include "themeload.h"

#include <pango/pangocairo.h>
#include <string.h>

#ifdef BT_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#include <pango/pangofc-fontmap.h>
#endif

#ifdef G_OS_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

struct BtThemeLoader {
  GdkDisplay *display;
  GtkCssProvider *provider;
  GFileMonitor *monitor;
  gchar *dev_path;
  guint reload_id;
  guint errors;
};

/* ------------------------------------------------------------------ fonts -- */

gchar *bt_themeload_exe_dir(void)
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

static gchar *find_fonts_dir(void)
{
  g_autofree gchar *exe_dir = bt_themeload_exe_dir();
  g_autoptr(GPtrArray) cands = g_ptr_array_new_with_free_func(g_free);
  if (exe_dir != NULL) {
    g_ptr_array_add(cands, g_build_filename(exe_dir, "share", "brainterm", "fonts", NULL));
    g_ptr_array_add(cands, g_build_filename(exe_dir, "..", "share", "brainterm", "fonts", NULL));
    g_ptr_array_add(cands, g_build_filename(exe_dir, "..", "..", "data", "fonts", NULL));
  }
  g_autofree gchar *cwd = g_get_current_dir();
  g_ptr_array_add(cands, g_build_filename(cwd, "data", "fonts", NULL));
  const gchar *const *sys = g_get_system_data_dirs();
  for (guint i = 0; sys != NULL && sys[i] != NULL; i++)
    g_ptr_array_add(cands, g_build_filename(sys[i], "brainterm", "fonts", NULL));

  for (guint i = 0; i < cands->len; i++) {
    const gchar *d = g_ptr_array_index(cands, i);
    if (g_file_test(d, G_FILE_TEST_IS_DIR))
      return g_strdup(d);
  }
  return NULL;
}

void bt_fonts_register(void)
{
  static gboolean done = FALSE;
  if (done)
    return;
  done = TRUE;

  g_autofree gchar *dir = find_fonts_dir();
  if (dir == NULL)
    return;

  GDir *d = g_dir_open(dir, 0, NULL);
  if (d == NULL)
    return;

  GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
  const gchar *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    if (g_str_has_suffix(name, ".ttf") || g_str_has_suffix(name, ".otf"))
      g_ptr_array_add(files, g_build_filename(dir, name, NULL));
  }
  g_dir_close(d);

#ifdef G_OS_WIN32
  /* Private (process-lifetime) fonts for the GDI/DirectWrite Pango backends. */
  for (guint i = 0; i < files->len; i++) {
    wchar_t *w = (wchar_t *)g_utf8_to_utf16(g_ptr_array_index(files, i), -1, NULL, NULL, NULL);
    if (w != NULL) {
      AddFontResourceExW(w, FR_PRIVATE, 0);
      g_free(w);
    }
  }
#endif

#ifdef BT_HAVE_FONTCONFIG
  PangoFontMap *fm = pango_cairo_font_map_get_default();
  if (PANGO_IS_FC_FONT_MAP(fm)) {
    for (guint i = 0; i < files->len; i++)
      FcConfigAppFontAddFile(NULL, (const FcChar8 *)g_ptr_array_index(files, i));
    pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(fm));
  }
#endif
  g_ptr_array_unref(files);
}

gboolean bt_fonts_family_available(const gchar *family)
{
  PangoFontMap *fm = pango_cairo_font_map_get_default();
  if (fm == NULL || family == NULL)
    return FALSE;
  PangoFontFamily **fams = NULL;
  int n = 0;
  pango_font_map_list_families(fm, &fams, &n);
  gboolean found = FALSE;
  for (int i = 0; i < n && !found; i++)
    found = g_ascii_strcasecmp(pango_font_family_get_name(fams[i]), family) == 0;
  g_free(fams);
  return found;
}

/* -------------------------------------------------------------------- CSS -- */

static void on_css_error(GtkCssProvider *p, GtkCssSection *section, GError *error, gpointer ud)
{
  (void)p;
  BtThemeLoader *l = ud;
  l->errors++;
  const GtkCssLocation *loc = section ? gtk_css_section_get_start_location(section) : NULL;
  g_printerr("theme.css:%u: %s\n", loc ? (guint)loc->lines + 1u : 0u, error->message);
}

static void load_stylesheet(BtThemeLoader *l)
{
  l->errors = 0;
  if (l->dev_path != NULL)
    gtk_css_provider_load_from_path(l->provider, l->dev_path);
  else
    gtk_css_provider_load_from_resource(l->provider, BT_THEME_RESOURCE_PATH);
}

static gboolean reload_timeout(gpointer ud)
{
  BtThemeLoader *l = ud;
  l->reload_id = 0;
  load_stylesheet(l);
  g_printerr("theme.css reloaded (%u parse error(s))\n", l->errors);
  return G_SOURCE_REMOVE;
}

static void on_css_file_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev,
                                gpointer ud)
{
  (void)m; (void)f; (void)other;
  BtThemeLoader *l = ud;
  if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && ev != G_FILE_MONITOR_EVENT_CHANGED &&
      ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_RENAMED)
    return;
  if (l->reload_id != 0)
    g_source_remove(l->reload_id);
  l->reload_id = g_timeout_add(80, reload_timeout, l); /* debounce editor saves */
}

BtThemeLoader *bt_themeloader_new(GdkDisplay *display)
{
  if (display == NULL)
    display = gdk_display_get_default();
  if (display == NULL)
    return NULL;

  BtThemeLoader *l = g_new0(BtThemeLoader, 1);
  l->display = display;
  l->provider = gtk_css_provider_new();
  g_signal_connect(l->provider, "parsing-error", G_CALLBACK(on_css_error), l);

  const gchar *dev = g_getenv(BT_CSS_DEV_ENV);
  if (dev != NULL && dev[0] != '\0' && g_file_test(dev, G_FILE_TEST_IS_REGULAR)) {
    l->dev_path = g_strdup(dev);
    GFile *f = g_file_new_for_path(l->dev_path);
    l->monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(f);
    if (l->monitor != NULL)
      g_signal_connect(l->monitor, "changed", G_CALLBACK(on_css_file_changed), l);
    g_printerr("theme.css: DEV mode, hot-reloading %s\n", l->dev_path);
  }

  load_stylesheet(l);
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(l->provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(display), BT_ICON_RESOURCE_PATH);
  return l;
}

void bt_themeloader_free(BtThemeLoader *l)
{
  if (l == NULL)
    return;
  if (l->reload_id != 0)
    g_source_remove(l->reload_id);
  if (l->monitor != NULL) {
    g_signal_handlers_disconnect_by_data(l->monitor, l);
    g_object_unref(l->monitor);
  }
  gtk_style_context_remove_provider_for_display(l->display, GTK_STYLE_PROVIDER(l->provider));
  g_object_unref(l->provider);
  g_free(l->dev_path);
  g_free(l);
}

guint bt_themeloader_error_count(const BtThemeLoader *l)
{
  return l ? l->errors : 0;
}

gboolean bt_themeloader_is_dev(const BtThemeLoader *l)
{
  return l != NULL && l->dev_path != NULL;
}
