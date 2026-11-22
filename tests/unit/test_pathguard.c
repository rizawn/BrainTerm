/* test_pathguard.c — §11.1 hostile-path table for vault_resolve() (§6.A4). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "vault.h"

typedef struct {
  gchar *base;    /* temp parent */
  gchar *root;    /* base/vault */
  gchar *sibling; /* base/vault2 — shares the root as a string prefix */
  BtVault *vault;
} Fixture;

static void write_file(const gchar *path, const gchar *text)
{
  assert_true(g_file_set_contents(path, text, -1, NULL));
}

static void rm_rf(const gchar *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if (d != NULL) {
    const gchar *n;
    while ((n = g_dir_read_name(d)) != NULL) {
      gchar *c = g_build_filename(path, n, NULL);
      if (g_file_test(c, G_FILE_TEST_IS_DIR) && !g_file_test(c, G_FILE_TEST_IS_SYMLINK))
        rm_rf(c);
      else
        g_remove(c);
      g_free(c);
    }
    g_dir_close(d);
  }
  g_rmdir(path);
}

static int setup(void **state)
{
  Fixture *f = g_new0(Fixture, 1);
  f->base = g_dir_make_tmp("bt-guard-XXXXXX", NULL);
  assert_non_null(f->base);
  f->root = g_build_filename(f->base, "vault", NULL);
  f->sibling = g_build_filename(f->base, "vault2", NULL);
  gchar *sub = g_build_filename(f->root, "a", "b", NULL);
  assert_int_equal(g_mkdir_with_parents(sub, 0755), 0);
  assert_int_equal(g_mkdir_with_parents(f->sibling, 0755), 0);
  gchar *note = g_build_filename(sub, "n.md", NULL);
  write_file(note, "# n\n");
  g_free(note);
  g_free(sub);
  f->vault = vault_open(f->root, NULL, NULL);
  assert_non_null(f->vault);
  *state = f;
  return 0;
}

static int teardown(void **state)
{
  Fixture *f = *state;
  vault_free(f->vault);
  rm_rf(f->base);
  g_free(f->base);
  g_free(f->root);
  g_free(f->sibling);
  g_free(f);
  return 0;
}

static void expect_rejected(BtVault *v, const gchar *rel)
{
  GError *err = NULL;
  gchar *abs = vault_resolve(v, rel, &err);
  if (abs != NULL)
    fail_msg("accepted hostile path '%s' -> '%s'", rel ? rel : "(null)", abs);
  assert_non_null(err);
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  /* The message must name the offending path (§6.A4). */
  if (rel != NULL && g_utf8_validate(rel, -1, NULL))
    assert_non_null(strstr(err->message, rel));
  g_error_free(err);
}

static void expect_inside(Fixture *f, const gchar *rel, const gchar *suffix)
{
  GError *err = NULL;
  gchar *abs = vault_resolve(f->vault, rel, &err);
  if (abs == NULL)
    fail_msg("rejected legit path '%s': %s", rel, err ? err->message : "?");
  const gchar *root = vault_root(f->vault);
  assert_true(g_str_has_prefix(abs, root));
  assert_true(strlen(abs) > strlen(root));
  gchar *want = g_build_filename(root, suffix, NULL);
  assert_string_equal(abs, want);
  g_free(want);
  g_free(abs);
}

static void test_hostile_table(void **state)
{
  Fixture *f = *state;
  static const gchar *const hostile[] = {
    "",
    "..",
    "../x.md",
    "../../x.md",
    "a/../../x.md",
    "a/b/../../../x.md",
    "./../x.md",
    "a/./../../x.md",
    "../vault2/x.md", /* sibling sharing the root string prefix */
    "/etc/passwd",
    "/x.md",
    "\\x.md",
    "//server/share/x.md",
    ".",
    "a/..",
#ifdef G_OS_WIN32
    "..\\x.md",
    "a\\..\\..\\x.md",
    "C:\\Windows\\win.ini",
    "C:/Windows/win.ini",
    "C:x.md",
    "c:..\\x.md",
    "\\\\server\\share\\x.md",
    "\\\\?\\C:\\x.md",
    "\\\\.\\PhysicalDrive0",
    "note.md:stream",
    "CON",
    "con.md",
    "a/NUL.md",
    "a/com1.txt.md",
    "LPT9 .md",
#endif
    NULL,
  };
  for (guint i = 0; hostile[i] != NULL; i++)
    expect_rejected(f->vault, hostile[i]);
  expect_rejected(f->vault, NULL);
  expect_rejected(f->vault, "bad\xff\xfe.md");

  gchar *abs_note = g_build_filename(vault_root(f->vault), "a", "b", "n.md", NULL);
  expect_rejected(f->vault, abs_note); /* absolute path smuggling, even inside */
  g_free(abs_note);
}

static void test_legit_paths(void **state)
{
  Fixture *f = *state;
  gchar *ab = g_build_filename("a", "b", "n.md", NULL);
  expect_inside(f, "a/b/n.md", ab);
  expect_inside(f, "a/./b/n.md", ab);
  expect_inside(f, "a/x/../b/n.md", ab);
  g_free(ab);
  gchar *newp = g_build_filename("new", "deep", "z.md", NULL);
  expect_inside(f, "new/deep/z.md", newp); /* not existing yet: allowed */
  g_free(newp);
  expect_inside(f, "..foo.md", "..foo.md"); /* dots in a name are not traversal */
  expect_inside(f, "x..md", "x..md");
}

static gboolean make_dir_link(const gchar *link, const gchar *target)
{
#ifdef G_OS_WIN32
  wchar_t *wl = (wchar_t *)g_utf8_to_utf16(link, -1, NULL, NULL, NULL);
  wchar_t *wt = (wchar_t *)g_utf8_to_utf16(target, -1, NULL, NULL, NULL);
  BOOLEAN ok = CreateSymbolicLinkW(wl, wt, SYMBOLIC_LINK_FLAG_DIRECTORY |
                                               SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  g_free(wl);
  g_free(wt);
  return ok != 0;
#else
  return symlink(target, link) == 0;
#endif
}

static void test_symlink_escape(void **state)
{
  Fixture *f = *state;
  gchar *link = g_build_filename(vault_root(f->vault), "esc", NULL);
  gchar *inner_link = g_build_filename(vault_root(f->vault), "inner", NULL);
  gchar *inner_target = g_build_filename(vault_root(f->vault), "a", NULL);
  if (!make_dir_link(link, f->sibling)) {
    g_free(link);
    g_free(inner_link);
    g_free(inner_target);
    skip(); /* Windows without Developer Mode cannot create symlinks */
    return;
  }
  expect_rejected(f->vault, "esc/x.md");
  expect_rejected(f->vault, "esc/new/deeper/x.md");
  expect_rejected(f->vault, "esc");

  /* A chain: inner -> a (inside), legit; chain through esc stays rejected. */
  assert_true(make_dir_link(inner_link, inner_target));
  gchar *want = g_build_filename("a", "b", "n.md", NULL);
  expect_inside(f, "inner/b/n.md", want);
  g_free(want);

  /* The scanner must not descend into links either. */
  gchar *outside_note = g_build_filename(f->sibling, "leak.md", NULL);
  write_file(outside_note, "secret");
  g_free(outside_note);
  assert_true(vault_rescan(f->vault, NULL, NULL));
  const GPtrArray *notes = vault_notes(f->vault);
  for (guint i = 0; i < notes->len; i++)
    assert_null(strstr(g_ptr_array_index(notes, i), "leak"));

#ifdef G_OS_WIN32
  RemoveDirectoryA(link);
  RemoveDirectoryA(inner_link);
#endif
  g_free(link);
  g_free(inner_link);
  g_free(inner_target);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_hostile_table, setup, teardown),
    cmocka_unit_test_setup_teardown(test_legit_paths, setup, teardown),
    cmocka_unit_test_setup_teardown(test_symlink_escape, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
