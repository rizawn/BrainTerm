/* test_atomicwrite.c — §6.A3 atomic write behavior (non-crash paths). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "testutil.h"
#include "vault.h"

typedef struct {
  gchar *root;
  BtVault *vault;
} Fixture;

static int setup(void **state)
{
  Fixture *f = g_new0(Fixture, 1);
  f->root = g_dir_make_tmp("bt-aw-XXXXXX", NULL);
  assert_non_null(f->root);
  f->vault = vault_open(f->root, NULL, NULL);
  assert_non_null(f->vault);
  *state = f;
  return 0;
}

static int teardown(void **state)
{
  Fixture *f = *state;
  vault_free(f->vault);
  bt_test_rm_rf(f->root);
  g_free(f->root);
  g_free(f);
  return 0;
}

static void assert_content(Fixture *f, const gchar *rel, const gchar *want, gsize want_len)
{
  gchar *got = NULL;
  gsize len = 0;
  assert_true(vault_read(f->vault, rel, &got, &len, NULL));
  assert_int_equal(len, want_len);
  assert_memory_equal(got, want, want_len);
  g_free(got);
}

static void assert_no_tmp(Fixture *f, const gchar *rel)
{
  gchar *abs = vault_resolve(f->vault, rel, NULL);
  gchar *tmp = g_strconcat(abs, ".tmp", NULL);
  assert_false(g_file_test(tmp, G_FILE_TEST_EXISTS));
  g_free(tmp);
  g_free(abs);
}

static void test_create_and_overwrite(void **state)
{
  Fixture *f = *state;
  const gchar *a = "---\ntitle: x\n---\nhello\r\nworld\n";
  assert_true(vault_atomic_write(f->vault, "n.md", a, strlen(a), NULL, NULL));
  assert_content(f, "n.md", a, strlen(a)); /* CRLF preserved byte-exact */
  assert_no_tmp(f, "n.md");

  gsize blen = 0;
  gchar *b = bt_test_payload("NEW", 200000, &blen);
  assert_true(vault_atomic_write(f->vault, "n.md", b, blen, NULL, NULL));
  assert_content(f, "n.md", b, blen);
  assert_no_tmp(f, "n.md");
  g_free(b);

  assert_true(vault_atomic_write(f->vault, "empty.md", "", 0, NULL, NULL));
  assert_content(f, "empty.md", "", 0);
}

static void test_creates_parent_dirs(void **state)
{
  Fixture *f = *state;
  assert_true(vault_atomic_write(f->vault, "inbox/terminal/x.md", "x", 1, NULL, NULL));
  assert_content(f, "inbox/terminal/x.md", "x", 1);
}

static void test_stale_tmp_is_replaced(void **state)
{
  Fixture *f = *state;
  bt_test_put(f->root, "s.md", "old");
  bt_test_put(f->root, "s.md.tmp", "garbage from a crash");
  assert_true(vault_atomic_write(f->vault, "s.md", "new", 3, NULL, NULL));
  assert_content(f, "s.md", "new", 3);
  assert_no_tmp(f, "s.md");
}

static void test_rejects_hostile_targets(void **state)
{
  Fixture *f = *state;
  GError *err = NULL;
  assert_false(vault_atomic_write(f->vault, "../escape.md", "x", 1, NULL, &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  g_clear_error(&err);
  gchar *parent = g_path_get_dirname(vault_root(f->vault));
  gchar *escaped = g_build_filename(parent, "escape.md", NULL);
  assert_false(g_file_test(escaped, G_FILE_TEST_EXISTS));
  g_free(escaped);
  g_free(parent);

  assert_false(vault_atomic_write(f->vault, "x.md.tmp", "x", 1, NULL, &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  g_clear_error(&err);
}

typedef struct {
  guint seen_mask;
  Fixture *f;
} StageProbe;

static void probe_stage(BtWriteStage stage, void *ud)
{
  StageProbe *p = ud;
  p->seen_mask |= 1u << stage;
  gchar *tmp = g_build_filename(p->f->root, "p.md.tmp", NULL);
  gboolean tmp_exists = g_file_test(tmp, G_FILE_TEST_EXISTS);
  g_free(tmp);
  /* Temp exists from open until rename, and is gone after. */
  if (stage == BT_WRITE_STAGE_AFTER_RENAME)
    assert_false(tmp_exists);
  else
    assert_true(tmp_exists);
}

static void test_stage_order(void **state)
{
  Fixture *f = *state;
  StageProbe p = {0, f};
  BtWriteHooks hooks = {probe_stage, &p};
  assert_true(vault_atomic_write(f->vault, "p.md", "abcdef", 6, &hooks, NULL));
  assert_int_equal(p.seen_mask, (1u << BT_WRITE_STAGE_COUNT) - 1);
}

static void test_rename_note_basic_and_cross_dir(void **state)
{
  Fixture *f = *state;
  GError *err = NULL;
  assert_true(vault_atomic_write(f->vault, "old.md", "# hello\n", 8, NULL, &err));
  assert_null(err);

  /* Same-dir rename */
  assert_true(vault_rename_note(f->vault, "old.md", "renamed.md", &err));
  assert_null(err);
  assert_content(f, "renamed.md", "# hello\n", 8);
  gchar *old_abs = vault_resolve(f->vault, "old.md", NULL);
  assert_false(g_file_test(old_abs, G_FILE_TEST_EXISTS));
  g_free(old_abs);

  /* Cross-folder move (auto-creates parent directory) */
  assert_true(vault_rename_note(f->vault, "renamed.md", "notes/sub/moved.md", &err));
  assert_null(err);
  assert_content(f, "notes/sub/moved.md", "# hello\n", 8);

  /* Case-only rename (NTFS two-step hop on Windows, direct on POSIX) */
  assert_true(vault_rename_note(f->vault, "notes/sub/moved.md", "notes/sub/Moved.md", &err));
  assert_null(err);
  assert_content(f, "notes/sub/Moved.md", "# hello\n", 8);

  /* No-op identical rename */
  assert_true(vault_rename_note(f->vault, "notes/sub/Moved.md", "notes/sub/Moved.md", &err));
  assert_null(err);
}

static void test_rename_note_rejections(void **state)
{
  Fixture *f = *state;
  GError *err = NULL;
  assert_true(vault_atomic_write(f->vault, "a.md", "A", 1, NULL, NULL));
  assert_true(vault_atomic_write(f->vault, "b.md", "B", 1, NULL, NULL));

  /* Refuses overwriting an existing distinct note */
  assert_false(vault_rename_note(f->vault, "a.md", "b.md", &err));
  assert_non_null(err);
  g_clear_error(&err);
  assert_content(f, "a.md", "A", 1);
  assert_content(f, "b.md", "B", 1);

  /* Refuses non-.md destination */
  assert_false(vault_rename_note(f->vault, "a.md", "a.txt", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  g_clear_error(&err);

  /* Refuses path traversal */
  assert_false(vault_rename_note(f->vault, "a.md", "../escape.md", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  g_clear_error(&err);

  /* Missing source */
  assert_false(vault_rename_note(f->vault, "missing.md", "x.md", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_NOT_FOUND));
  g_clear_error(&err);
}

static void test_delete_note(void **state)
{
  Fixture *f = *state;
  GError *err = NULL;
  assert_true(vault_atomic_write(f->vault, "sub/delme.md", "bye", 3, NULL, NULL));
  assert_true(vault_delete_note(f->vault, "sub/delme.md", &err));
  assert_null(err);

  gchar *abs = vault_resolve(f->vault, "sub/delme.md", NULL);
  assert_false(g_file_test(abs, G_FILE_TEST_EXISTS));
  g_free(abs);

  /* Missing file */
  assert_false(vault_delete_note(f->vault, "sub/delme.md", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_NOT_FOUND));
  g_clear_error(&err);

  /* Directory refusal */
  assert_false(vault_delete_note(f->vault, "sub", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_INVALID));
  g_clear_error(&err);

  /* Traversal refusal */
  assert_false(vault_delete_note(f->vault, "../outside.md", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  g_clear_error(&err);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_create_and_overwrite, setup, teardown),
    cmocka_unit_test_setup_teardown(test_creates_parent_dirs, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stale_tmp_is_replaced, setup, teardown),
    cmocka_unit_test_setup_teardown(test_rejects_hostile_targets, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stage_order, setup, teardown),
    cmocka_unit_test_setup_teardown(test_rename_note_basic_and_cross_dir, setup, teardown),
    cmocka_unit_test_setup_teardown(test_rename_note_rejections, setup, teardown),
    cmocka_unit_test_setup_teardown(test_delete_note, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
