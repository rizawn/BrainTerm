/* test_sessions.c — M3 session persistence via vault/.bt/sessions.json. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "agents.h"
#include "sessions.h"
#include "testutil.h"

static void test_sessions_roundtrip(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-sess-XXXXXX", NULL);
  BtSessionStore *st = bt_sessions_open(root, NULL);
  assert_non_null(st);

  gchar *resume = bt_agents_format_resume_cmd("claude-code", "abc-123");
  assert_string_equal(resume, "claude --resume abc-123\r");

  const BtSession *s = bt_sessions_add(st, "claude-code", root, resume, "welcome.md",
                                       "0123456789abcdef0123456789abcdef01234567", NULL);
  assert_non_null(s);
  gchar *sid = g_strdup(s->id);
  assert_int_equal(s->ended, 0);
  assert_true(bt_sessions_mark_ended(st, sid, NULL));
  bt_sessions_free(st);
  g_free(resume);

  /* Re-open from disk and verify round-trip. */
  BtSessionStore *st2 = bt_sessions_open(root, NULL);
  assert_int_equal(bt_sessions_list(st2)->len, 1);
  const BtSession *s2 = bt_sessions_find(st2, sid);
  assert_non_null(s2);
  assert_string_equal(s2->tool, "claude-code");
  assert_string_equal(s2->note_id, "welcome.md");
  assert_string_equal(s2->checkpoint_oid, "0123456789abcdef0123456789abcdef01234567");
  assert_true(s2->ended > 0);

  gchar *codex_r = bt_agents_format_resume_cmd("codex", NULL);
  assert_string_equal(codex_r, "codex resume\r");
  g_free(codex_r);

  g_free(sid);
  bt_sessions_free(st2);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sessions_roundtrip),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
