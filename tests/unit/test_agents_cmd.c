/* test_agents_cmd.c — §8.1 hostile note titles never inject shell metachars,
 * and vault-template never overwrites existing AGENTS.md. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "agents.h"
#include "search.h"
#include "testutil.h"

static void test_hostile_titles(void **state)
{
  (void)state;
  const gchar *hostile = "evil\"; 'rm' %USERPROFILE% !PATH! $(whoami) `id`\n\r|cat .env";
  gchar *cmd = bt_agents_format_launch_cmd(BT_AGENT_CLAUDE, hostile);
  assert_true(g_str_has_prefix(cmd, "claude \"Read "));
  assert_true(g_str_has_suffix(cmd, " as context.\"\r"));

  /* Inside the quotes, zero shell metacharacters or raw newlines may appear. */
  const gchar *inner_start = strchr(cmd, '"') + 1;
  const gchar *inner_end = strrchr(cmd, '"');
  assert_true(inner_end > inner_start);
  for (const gchar *p = inner_start; p < inner_end; p++) {
    assert_true(*p != '"' && *p != '\'' && *p != '`' && *p != '$' && *p != '%' && *p != '!' &&
                *p != '\\' && *p != '\n' && *p != '\r' && *p != ';' && *p != '|' && *p != '&');
  }
  g_free(cmd);

  gchar *codex = bt_agents_format_launch_cmd(BT_AGENT_CODEX, "ConPTY Notes");
  assert_string_equal(codex, "codex \"Read ConPTY Notes as context.\"\r");
  g_free(codex);

  gchar *gemini = bt_agents_format_launch_cmd(BT_AGENT_GEMINI, "Daily");
  assert_string_equal(gemini, "gemini \"Read Daily as context.\"\r");
  g_free(gemini);

  gchar *ask = bt_search_compose_ask_agent_cmd_for(
      "claude", "why %USERPROFILE% 'quote' !bang $(id)", NULL, 3);
  assert_non_null(ask);
  assert_null(strchr(ask, '%'));
  assert_null(strchr(ask, '\''));
  assert_null(strchr(ask, '!'));
  assert_null(strchr(ask, '$'));
  g_free(ask);
}

static void test_vault_template_no_overwrite(void **state)
{
  (void)state;
  gchar *root = g_dir_make_tmp("bt-tpl-XXXXXX", NULL);
  BtVault *v = vault_open(root, NULL, NULL);
  assert_non_null(v);

  gboolean differs = TRUE;
  assert_true(bt_agents_apply_vault_template(v, &differs, NULL));
  assert_false(differs);

  gchar *agents_txt = NULL;
  assert_true(vault_read(v, "AGENTS.md", &agents_txt, NULL, NULL));
  assert_string_equal(agents_txt, bt_agents_template_agents_md());
  g_free(agents_txt);

  /* Customize AGENTS.md -> re-applying MUST NOT overwrite it and must flag differs. */
  const gchar *custom = "# Custom user rules\n";
  assert_true(vault_atomic_write(v, "AGENTS.md", custom, strlen(custom), NULL, NULL));
  assert_true(bt_agents_apply_vault_template(v, &differs, NULL));
  assert_true(differs);

  assert_true(vault_read(v, "AGENTS.md", &agents_txt, NULL, NULL));
  assert_string_equal(agents_txt, custom);
  g_free(agents_txt);

  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_hostile_titles),
    cmocka_unit_test(test_vault_template_no_overwrite),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
