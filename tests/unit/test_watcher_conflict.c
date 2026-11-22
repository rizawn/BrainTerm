/* test_watcher_conflict.c — M5 watcher debounce, R4 storm guard, R3 drift
 * audit, and §6.A5 conflict banner decision matrix (simulated-AI AC). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "checkpoint.h"
#include "frontmatter.h"
#include "index.h"
#include "testutil.h"
#include "vault.h"
#include "watcher.h"

typedef struct {
  GMainLoop *loop;
  BtWatchEventKind last_kind;
  gchar *last_path;
  guint count;
} WatchProbe;

static void on_watch(BtWatchEventKind kind, const gchar *relpath, gpointer ud)
{
  WatchProbe *p = ud;
  p->last_kind = kind;
  g_free(p->last_path);
  p->last_path = g_strdup(relpath);
  p->count++;
  g_main_loop_quit(p->loop);
}

static gboolean on_tmo(gpointer ud)
{
  g_main_loop_quit(ud);
  return G_SOURCE_REMOVE;
}

static void test_watcher_storm_and_r3_audit(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-watch-XXXXXX", NULL);
  bt_test_put(root, "a.md", "# Alpha\ninitial\n");
  BtVault *v = vault_open(root, NULL, NULL);
  BtIndex *idx = bt_index_open(root, NULL, NULL);
  assert_true(bt_index_rebuild(idx, v, NULL));

  /* Self-save no-op via hash equality. */
  gboolean skipped = FALSE;
  assert_true(bt_index_upsert_note(idx, v, "a.md", &skipped, NULL));
  assert_true(skipped);

  /* R4 storm guard: > 500 events triggers BT_WATCH_STORM_RESCAN. */
  WatchProbe wp = {.loop = g_main_loop_new(NULL, FALSE)};
  BtWatcher *w = bt_watcher_new(v, 50, on_watch, &wp);
  for (guint i = 0; i < BT_WATCHER_STORM_THRESHOLD + 10; i++) {
    gchar *rel = g_strdup_printf("n%u.md", i);
    bt_watcher_inject_event(w, rel, FALSE);
    g_free(rel);
  }
  guint tid = g_timeout_add(1000, on_tmo, wp.loop);
  g_main_loop_run(wp.loop);
  if (wp.count > 0)
    g_source_remove(tid);
  assert_int_equal(wp.count, 1);
  assert_int_equal(wp.last_kind, BT_WATCH_STORM_RESCAN);
  bt_watcher_free(w);

  /* R3 differential audit: external AI edits a.md while watcher is off. */
  bt_test_put(root, "a.md", "# Alpha\nmodified externally by AI\n");
  GPtrArray *drifted = NULL;
  guint fixed = bt_watcher_audit_drift(v, idx, &drifted);
  assert_int_equal(fixed, 1);
  assert_string_equal(g_ptr_array_index(drifted, 0), "a.md");
  assert_int_equal(bt_index_count_fts_matches(idx, "externally"), 1);
  g_ptr_array_unref(drifted);

  g_free(wp.last_path);
  g_main_loop_unref(wp.loop);
  bt_index_close(idx);
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

static void test_conflict_branches_and_checkpoint_diff(void **state)
{
  (void)state;
  gchar *h_same = bt_fnv1a_hex("abc", 3);
  gchar *h_diff = bt_fnv1a_hex("xyz", 3);

  assert_int_equal(bt_conflict_decide(FALSE, FALSE, h_same, h_same), BT_CONFLICT_NOOP);
  assert_int_equal(bt_conflict_decide(TRUE, FALSE, h_same, h_same), BT_CONFLICT_NOOP);
  assert_int_equal(bt_conflict_decide(FALSE, FALSE, h_same, h_diff), BT_CONFLICT_SILENT_RELOAD);
  assert_int_equal(bt_conflict_decide(TRUE, FALSE, h_same, h_diff), BT_CONFLICT_BANNER_DIRTY);
  assert_int_equal(bt_conflict_decide(FALSE, TRUE, h_same, NULL), BT_CONFLICT_BANNER_DELETED);
  assert_int_equal(bt_conflict_decide(TRUE, TRUE, h_same, NULL), BT_CONFLICT_BANNER_DELETED);

  /* Verify Review diff can retrieve pre-agent version from checkpoint. */
  bt_checkpoint_init();
  gchar *root = g_dir_make_tmp("bt-cdiff-XXXXXX", NULL);
  bt_test_put(root, "note.md", "human v1\n");
  gchar oid[BT_OID_HEX_LEN] = {0};
  assert_true(bt_checkpoint_take(root, "pre-agent", oid, NULL));
  bt_test_put(root, "note.md", "agent v2\n");
  gchar *cp_ver = NULL;
  assert_true(bt_checkpoint_read_file(root, "HEAD", "note.md", &cp_ver, NULL, NULL));
  assert_string_equal(cp_ver, "human v1\n");
  g_free(cp_ver);
  bt_test_rm_rf(root);
  g_free(root);
  bt_checkpoint_shutdown();

  g_free(h_same);
  g_free(h_diff);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_watcher_storm_and_r3_audit),
    cmocka_unit_test(test_conflict_branches_and_checkpoint_diff),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
