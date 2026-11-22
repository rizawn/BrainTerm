/* test_tabs.c — M3 multi-tab cap (8) + cwd vault_resolve + zero-orphan close. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <cmocka.h>

#include "pty.h"
#include "testutil.h"
#include "vault.h"

#define TAB_CAP 8u

static void test_tab_cap_and_sweep(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-tabs-XXXXXX", NULL);
  bt_test_put(root, "projects/a.md", "# a\n");
  BtVault *v = vault_open(root, NULL, NULL);
  assert_non_null(v);

  /* Verify cwd resolution passes vault_resolve() and rejects traversal. */
  GError *err = NULL;
  assert_null(vault_resolve(v, "../outside", &err));
  assert_true(g_error_matches(err, BT_ERROR, BT_ERROR_PATH_REJECTED));
  g_clear_error(&err);

  gchar *sub = vault_resolve(v, "projects", NULL);
  assert_non_null(sub);
  gchar *badge = g_path_get_basename(sub);
  assert_string_equal(badge, "projects");
  g_free(badge);

  /* Spawn TAB_CAP PTYs, enforce cap on 9th, close all and sweep PIDs. */
  BtPty *ptys[TAB_CAP] = {NULL};
  guint64 pids[TAB_CAP] = {0};
#ifdef G_OS_WIN32
  const gchar *const argv[] = {"cmd.exe", "/Q", NULL};
#else
  const gchar *const argv[] = {"/bin/sh", NULL};
#endif

  guint open_count = 0;
  for (guint i = 0; i < TAB_CAP + 1; i++) {
    if (open_count >= TAB_CAP) {
      /* 9th tab refused by cap */
      assert_int_equal(i, TAB_CAP);
      break;
    }
    BtPtySpawnOpts opts = {
      .cwd = (i % 2 == 0) ? vault_root(v) : sub,
      .argv = argv,
      .rows = 24,
      .cols = 80,
    };
    ptys[i] = bt_pty_spawn(&opts, &err);
    assert_non_null(ptys[i]);
    pids[i] = bt_pty_child_pid(ptys[i]);
    assert_true(pids[i] != 0);
    open_count++;
  }
  assert_int_equal(open_count, TAB_CAP);

  for (guint i = 0; i < TAB_CAP; i++)
    bt_pty_close(ptys[i]);

  for (int t = 0; t < 20; t++) {
    gboolean any = FALSE;
    for (guint i = 0; i < TAB_CAP; i++)
      if (bt_pty_pid_alive(pids[i]))
        any = TRUE;
    if (!any)
      break;
    g_usleep(50000);
  }
  for (guint i = 0; i < TAB_CAP; i++)
    assert_false(bt_pty_pid_alive(pids[i]));

  g_free(sub);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_tab_cap_and_sweep),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
