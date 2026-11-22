/* testutil.h — shared helpers for BrainTerm tests (header-only). */
#ifndef BT_TESTUTIL_H
#define BT_TESTUTIL_H

#include <glib.h>
#include <glib/gstdio.h>

static inline void bt_test_rm_rf(const gchar *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if (d != NULL) {
    const gchar *n;
    while ((n = g_dir_read_name(d)) != NULL) {
      gchar *c = g_build_filename(path, n, NULL);
      if (g_file_test(c, G_FILE_TEST_IS_DIR) && !g_file_test(c, G_FILE_TEST_IS_SYMLINK))
        bt_test_rm_rf(c);
      else
        g_remove(c);
      g_free(c);
    }
    g_dir_close(d);
  }
  g_rmdir(path);
}

/* Deterministic multi-MB payload so partial writes are observable. */
static inline gchar *bt_test_payload(const gchar *tag, guint lines, gsize *len_out)
{
  GString *s = g_string_sized_new((gsize)lines * 24);
  for (guint i = 0; i < lines; i++)
    g_string_append_printf(s, "%s-line-%07u\n", tag, i);
  if (len_out != NULL)
    *len_out = s->len;
  return g_string_free(s, FALSE);
}

static inline void bt_test_put(const gchar *dir, const gchar *rel, const gchar *text)
{
  gchar *p = g_build_filename(dir, rel, NULL);
  gchar *parent = g_path_get_dirname(p);
  g_mkdir_with_parents(parent, 0755);
  if (!g_file_set_contents(p, text, -1, NULL))
    g_error("cannot create fixture %s", p);
  g_free(parent);
  g_free(p);
}

#endif
