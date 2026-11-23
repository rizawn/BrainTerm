/* test_createnote.c — Groups CR (CR01..CR37), CN, CT, CW tests for Phase L3. */
#ifndef G_OS_WIN32
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>

#include "createnote.h"
#include "linkresolve.h"
#include "testutil.h"
#include "vault.h"

static gboolean create_outside_link(const char *link_path, const char *target_dir)
{
#ifdef G_OS_WIN32
  g_autofree gchar *cmd = g_strdup_printf("cmd.exe /c mklink /J \"%s\" \"%s\" >NUL 2>&1",
                                          link_path, target_dir);
  return system(cmd) == 0;
#else
  return symlink(target_dir, link_path) == 0;
#endif
}

typedef struct {
  gchar *parent;
  gchar *vdir;
  gchar *outside_dir;
  BtVault *vault;
  BtIndexSnapshot *snap;
} CrFixture;

static CrFixture *cr_fixture_new(void)
{
  CrFixture *fx = g_new0(CrFixture, 1);
  fx->parent = g_dir_make_tmp("bt-l3-cr-XXXXXX", NULL);
  fx->vdir = g_build_filename(fx->parent, "vault", NULL);
  fx->outside_dir = g_build_filename(fx->parent, "outside", NULL);
  g_mkdir_with_parents(fx->vdir, 0755);
  g_mkdir_with_parents(fx->outside_dir, 0755);

  bt_test_put(fx->vdir, "welcome.md", "# Welcome\n");
  bt_test_put(fx->vdir, "projects/conpty.md", "# ConPTY\n");
  bt_test_put(fx->vdir, "projects/notes/conpty.md", "# Nested ConPTY\n");
  bt_test_put(fx->vdir, "statistical-statistics-notes/MathStats.md", "# MathStats\n");
  bt_test_put(fx->vdir, "statistical-statistics-notes/chapter-01-descriptive/01-data-types-collection.md",
              "# Data Types\n");
  bt_test_put(fx->vdir, "assets/paper.pdf", "%PDF-1.4 fixture\n");
  bt_test_put(fx->vdir, "scripts/run.ps1", "Write-Output 'ok'\n");
  bt_test_put(fx->vdir, "notes/R\xC3\xA9sum\xC3\xA9.md", "# Resume NFC\n"); /* NFC */
  g_autofree gchar *folder_only = g_build_filename(fx->vdir, "folder-only", NULL);
  g_mkdir_with_parents(folder_only, 0755);
  g_autofree gchar *new_idea_dir = g_build_filename(fx->vdir, "new-idea", NULL);
  g_mkdir_with_parents(new_idea_dir, 0755);

  fx->vault = vault_open(fx->vdir, NULL, NULL);
  assert_non_null(fx->vault);
  fx->snap = bt_index_snapshot_from_vault(fx->vault);
  /* Explicitly register folder-only, assets/, assets/paper.pdf, scripts/run.ps1 in snapshot
   * matching §7.1 fixture table. */
  bt_index_snapshot_add_entry(fx->snap, "assets", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(fx->snap, "assets/paper.pdf", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(fx->snap, "scripts", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(fx->snap, "scripts/run.ps1", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(fx->snap, "folder-only", TRUE, NULL, NULL);
  bt_index_snapshot_finalize(fx->snap);
  return fx;
}

static void cr_fixture_free(CrFixture *fx)
{
  if (!fx)
    return;
  bt_index_snapshot_free(fx->snap);
  vault_free(fx->vault);
  if (g_getenv("BT_KEEP_TEST_VAULTS") == NULL)
    bt_test_rm_rf(fx->parent);
  g_free(fx->outside_dir);
  g_free(fx->vdir);
  g_free(fx->parent);
  g_free(fx);
}

static void assert_reason_contains(const BtCreatePlan *p, const char *needle, const char *tag)
{
  if (p->reason == NULL || strstr(p->reason, needle) == NULL)
    fail_msg("%s: expected reason containing '%s', got '%s'", tag, needle, p->reason ? p->reason : "(null)");
}

static void test_cr01_to_cr37_planner(void **state)
{
  (void)state;
  CrFixture *fx = cr_fixture_new();

  /* CR01 */
  BtCreatePlan *p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "new-idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.md");
  assert_false(p->has_folder_prefix);
  assert_false(p->needs_folder_create);
  bt_createnote_plan_free(p);

  /* CR02 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "projects/new-idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "projects/new-idea.md");
  assert_true(p->has_folder_prefix);
  assert_false(p->needs_folder_create);
  bt_createnote_plan_free(p);

  /* CR03 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "brand/new/deep");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "brand/new/deep.md");
  assert_true(p->needs_folder_create);
  assert_int_equal(p->missing_folders->len, 2);
  assert_string_equal(g_ptr_array_index(p->missing_folders, 0), "brand");
  assert_string_equal(g_ptr_array_index(p->missing_folders, 1), "brand/new");
  bt_createnote_plan_free(p);

  /* CR04 */
  p = bt_createnote_plan(fx->vault, fx->snap, "projects/conpty.md", "../new-idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.md");
  bt_createnote_plan_free(p);

  /* CR05 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "../../escape");
  assert_int_equal(p->result, BT_CREATE_PLAN_OUTSIDE_VAULT);
  bt_createnote_plan_free(p);

  /* CR06 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "new-idea.md");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.md");
  bt_createnote_plan_free(p);

  /* CR07 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "new-idea.markdown");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.markdown");
  bt_createnote_plan_free(p);

  /* CR08: bare name 'paper' plans paper.md in source folder despite assets/paper.pdf */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "paper");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "paper.md");
  bt_createnote_plan_free(p);

  /* CR09 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "assets/paper.pdf");
  assert_int_equal(p->result, BT_CREATE_PLAN_NOT_A_NOTE);
  assert_reason_contains(p, ".pdf", "CR09");
  bt_createnote_plan_free(p);

  /* CR10 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "scripts/run.ps1");
  assert_int_equal(p->result, BT_CREATE_PLAN_NOT_A_NOTE);
  assert_reason_contains(p, ".ps1", "CR10");
  bt_createnote_plan_free(p);

  /* CR11 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "assets/");
  assert_int_equal(p->result, BT_CREATE_PLAN_NOT_A_NOTE);
  assert_string_equal(p->reason, "Folder");
  bt_createnote_plan_free(p);

  /* CR12: conpty at root creates conpty.md and flags has_other_same_basename */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "conpty");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "conpty.md");
  assert_true(p->has_other_same_basename);
  bt_createnote_plan_free(p);

  /* CR13 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "welcome");
  assert_int_equal(p->result, BT_CREATE_PLAN_EXISTS);
  assert_string_equal(p->target_relpath, "welcome.md");
  bt_createnote_plan_free(p);

  /* CR14 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "Welcome");
#ifdef G_OS_WIN32
  assert_int_equal(p->result, BT_CREATE_PLAN_EXISTS);
  assert_string_equal(p->target_relpath, "welcome.md");
#else
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "Welcome.md");
#endif
  bt_createnote_plan_free(p);

  /* CR15: NFC normalization before exact-path check (both NFD and NFC bytes match notes/Résumé.md) */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "notes/Re\xCC\x81sume\xCC\x81");
  assert_int_equal(p->result, BT_CREATE_PLAN_EXISTS);
  assert_string_equal(p->target_relpath, "notes/R\xC3\xA9sum\xC3\xA9.md");
  bt_createnote_plan_free(p);
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "notes/R\xC3\xA9sum\xC3\xA9");
  assert_int_equal(p->result, BT_CREATE_PLAN_EXISTS);
  assert_string_equal(p->target_relpath, "notes/R\xC3\xA9sum\xC3\xA9.md");
  bt_createnote_plan_free(p);
  p = bt_createnote_plan(fx->vault, fx->snap, "notes/other.md", "Re\xCC\x81sume\xCC\x81");
  assert_int_equal(p->result, BT_CREATE_PLAN_EXISTS);
  assert_string_equal(p->target_relpath, "notes/R\xC3\xA9sum\xC3\xA9.md");
  bt_createnote_plan_free(p);

  /* CR16 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "CON");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_string_equal(p->reason, "Reserved Windows name");
  bt_createnote_plan_free(p);

  /* CR17 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "aux/x");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_string_equal(p->reason, "Reserved Windows name");
  bt_createnote_plan_free(p);

  /* CR18 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "name. ");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_string_equal(p->reason, "Name ends with a dot or space");
  bt_createnote_plan_free(p);

  /* CR19 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "a:b");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_reason_contains(p, ":", "CR19");
  bt_createnote_plan_free(p);

  /* CR20 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "a?b");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_reason_contains(p, "?", "CR20");
  bt_createnote_plan_free(p);

  /* CR21 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "a<b>");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_reason_contains(p, "<", "CR21");
  bt_createnote_plan_free(p);

  /* CR22 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "a\\|b");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "a.md");
  assert_string_equal(p->display, "b");
  bt_createnote_plan_free(p);

  /* CR23 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "/absolute/path");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_string_equal(p->reason, "Absolute paths are not allowed");
  bt_createnote_plan_free(p);

  /* CR24 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "C:\\Windows\\x");
  assert_int_equal(p->result, BT_CREATE_PLAN_OUTSIDE_VAULT);
  bt_createnote_plan_free(p);

  /* CR25 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "{{template}}");
  assert_int_equal(p->result, BT_CREATE_PLAN_PLACEHOLDER);
  assert_null(p->reason);
  assert_null(bt_createnote_plan_status_message(p));
  bt_createnote_plan_free(p);

  /* CR26 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_string_equal(p->reason, "Empty name");
  bt_createnote_plan_free(p);

  /* CR27 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "   ");
  assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
  assert_string_equal(p->reason, "Empty name");
  bt_createnote_plan_free(p);

  /* CR28 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "new-idea#Heading");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.md");
  assert_string_equal(p->anchor, "Heading");
  bt_createnote_plan_free(p);

  /* CR29 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "new-idea|Alias");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.md");
  assert_string_equal(p->display, "Alias");
  bt_createnote_plan_free(p);

  /* CR30 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "folder-only/");
  assert_int_equal(p->result, BT_CREATE_PLAN_NOT_A_NOTE);
  assert_string_equal(p->reason, "Folder");
  bt_createnote_plan_free(p);

  /* CR31: test both sides of segment limit (240 vs 241) and path limit (1024 vs 1025) */
  {
    gchar seg200[201];
    memset(seg200, 'a', 200);
    seg200[200] = '\0';
    g_autofree gchar *t200 = g_strdup_printf("deep/%s", seg200);
    p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", t200);
    assert_int_equal(p->result, BT_CREATE_PLAN_OK);
    bt_createnote_plan_free(p);

    gchar seg240[241];
    memset(seg240, 'b', 240);
    seg240[240] = '\0';
    g_autofree gchar *t240 = g_strdup_printf("deep/%s", seg240);
    p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", t240);
    assert_int_equal(p->result, BT_CREATE_PLAN_OK);
    bt_createnote_plan_free(p);

    gchar seg241[242];
    memset(seg241, 'c', 241);
    seg241[241] = '\0';
    g_autofree gchar *t241 = g_strdup_printf("deep/%s", seg241);
    p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", t241);
    assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
    assert_string_equal(p->reason, "Name too long");
    bt_createnote_plan_free(p);

    /* Total path limit: 4*200 + 218 = 1018 (+ 4 '/' + '.md' = 1025 > 1024 vs 1024 exact). */
    gchar f200[201];
    memset(f200, 'd', 200);
    f200[200] = '\0';
    gchar last217[218];
    memset(last217, 'e', 217);
    last217[217] = '\0';
    g_autofree gchar *p1024 = g_strdup_printf("%s/%s/%s/%s/%s.md", f200, f200, f200, f200, last217);
    assert_int_equal(strlen(p1024), 1024);
    p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", p1024);
    assert_int_equal(p->result, BT_CREATE_PLAN_OK);
    bt_createnote_plan_free(p);

    g_autofree gchar *p1025 = g_strdup_printf("%s/%s/%s/%s/x%s.md", f200, f200, f200, f200, last217);
    assert_int_equal(strlen(p1025), 1025);
    p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", p1025);
    assert_int_equal(p->result, BT_CREATE_PLAN_INVALID_NAME);
    assert_string_equal(p->reason, "Name too long");
    bt_createnote_plan_free(p);
  }

  /* CR32 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "\xF0\x9F\xA7\xA0 idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "\xF0\x9F\xA7\xA0 idea.md");
  bt_createnote_plan_free(p);

  /* CR33 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "trailing-nl\n");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "trailing-nl.md");
  bt_createnote_plan_free(p);

  /* CR34 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "projects\\new-idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "projects/new-idea.md");
  bt_createnote_plan_free(p);

  /* CR35 */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "projects//new-idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "projects/new-idea.md");
  bt_createnote_plan_free(p);

  /* CR36: folder new-idea/ exists, creating new-idea.md is allowed */
  p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "new-idea");
  assert_int_equal(p->result, BT_CREATE_PLAN_OK);
  assert_string_equal(p->target_relpath, "new-idea.md");
  bt_createnote_plan_free(p);

  /* CR37: target traverses symlink/junction pointing outside vault */
  {
    g_autofree gchar *junc = g_build_filename(fx->vdir, "escape_dir", NULL);
    gboolean linked = create_outside_link(junc, fx->outside_dir);
    if (linked) {
      p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "escape_dir/outside-note");
      assert_int_equal(p->result, BT_CREATE_PLAN_OUTSIDE_VAULT);
      bt_createnote_plan_free(p);
      g_rmdir(junc);
    } else {
      printf("BT_TEST_CR37 skipped: OS denied symlink/junction creation in test dir\n");
    }
  }

  /* Rule 11: disk file absent from snapshot -> PLAN_EXISTS + stale_index = TRUE */
  {
    bt_test_put(fx->vdir, "created-externally.md", "# External\n");
    p = bt_createnote_plan(fx->vault, fx->snap, "welcome.md", "created-externally");
    assert_int_equal(p->result, BT_CREATE_PLAN_EXISTS);
    assert_string_equal(p->target_relpath, "created-externally.md");
    assert_true(p->stale_index);
    g_autofree gchar *smsg = bt_createnote_plan_status_message(p);
    assert_string_equal(smsg, "Index refreshed");
    bt_createnote_plan_free(p);
  }

  printf("BT_TEST_CREATENOTE CR01..CR37 passed (vault=%s)\n", fx->parent);
  cr_fixture_free(fx);
}

static void test_cn_name_validation_helpers(void **state)
{
  (void)state;
  const char *bad_chars[] = {
    "a<b", "a>b", "a:b", "a\"b", "a|b", "a?b", "a*b", "a/b", "a\\b", "a\x01" "b", "a\x1F" "b", "foo\t"
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad_chars); i++) {
    g_autofree gchar *why = NULL;
    assert_false(bt_createnote_validate_name(bad_chars[i], &why));
    assert_non_null(why);
    assert_non_null(strstr(why, "Contains a character Windows does not allow:"));
  }

  const char *reserved[] = {
    "CON", "con", "PRN", "AUX", "NUL",
    "COM1", "COM5", "COM9", "LPT1", "LPT4", "LPT9",
    "CON.md", "con.txt", "NUL.tar.gz", "com1.markdown", "lpt9.log"
  };
  for (gsize i = 0; i < G_N_ELEMENTS(reserved); i++) {
    g_autofree gchar *why = NULL;
    assert_false(bt_createnote_validate_name(reserved[i], &why));
    assert_string_equal(why, "Reserved Windows name");
  }

  const char *trailing[] = { "foo.", "foo ", "foo. ", ".", ".." };
  for (gsize i = 0; i < G_N_ELEMENTS(trailing); i++) {
    g_autofree gchar *why = NULL;
    assert_false(bt_createnote_validate_name(trailing[i], &why));
    assert_string_equal(why, "Name ends with a dot or space");
  }

  const char *valid_names[] = {
    "CON1", "CONSOLE", "normal-note", "my note.md",
    "R\xC3\xA9sum\xC3\xA9",          /* NFC */
    "Re\xCC\x81sume\xCC\x81",        /* NFD */
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xF0\x9F\xA7\xA0"
  };
  for (gsize i = 0; i < G_N_ELEMENTS(valid_names); i++) {
    g_autofree gchar *why = NULL;
    assert_true(bt_createnote_validate_name(valid_names[i], &why));
    assert_null(why);
  }
  printf("BT_TEST_CN validation helpers passed\n");
}

int main(void)
{
  setlocale(LC_NUMERIC, "C");
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_cr01_to_cr37_planner),
    cmocka_unit_test(test_cn_name_validation_helpers),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
