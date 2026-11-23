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
#include "frontmatter.h"
#include "linkresolve.h"
#include "testutil.h"
#include "vault.h"

static const char *g_self_exe = NULL;

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

static void test_ct_template_rendering(void **state)
{
  (void)state;
  GError *err = NULL;

  /* Sample display name matches exact §6.1.5 key order and LF endings without BOM. */
  g_autofree gchar *doc = bt_createnote_render_template("bt-20261123-000042", "Empirical Bayes",
                                                        "2026-11-23T08:30:00+07:00", &err);
  assert_non_null(doc);
  assert_null(err);
  assert_null(strstr(doc, "\r\n"));
  assert_false((guchar)doc[0] == 0xEFu && (guchar)doc[1] == 0xBBu && (guchar)doc[2] == 0xBFu);
  const char *exp =
      "---\n"
      "id: bt-20261123-000042\n"
      "title: \"Empirical Bayes\"\n"
      "created: 2026-11-23T08:30:00+07:00\n"
      "updated: 2026-11-23T08:30:00+07:00\n"
      "source: human\n"
      "agent: null\n"
      "tags: []\n"
      "tags_ai: []\n"
      "links: []\n"
      "---\n\n"
      "# Empirical Bayes\n\n";
  assert_string_equal(doc, exp);

  /* Quote escaping for " and \ round-trips identically through libyaml parser. */
  g_autofree gchar *tricky = g_strdup("Quote \"test\" and \\ slash");
  g_autofree gchar *doc_esc = bt_createnote_render_template("bt-20261123-000043", tricky,
                                                            "2026-11-23T08:30:00+07:00", &err);
  assert_non_null(doc_esc);
  BtFrontmatter *fm = bt_frontmatter_parse(doc_esc, strlen(doc_esc));
  assert_true(fm->has_frontmatter);
  assert_false(fm->parse_error);
  assert_string_equal(fm->title, tricky);
  bt_frontmatter_free(fm);

  /* Empty / whitespace display names rejected. */
  assert_null(bt_createnote_render_template("bt-20261123-000044", "", NULL, &err));
  assert_non_null(err);
  g_clear_error(&err);
  assert_null(bt_createnote_render_template("bt-20261123-000044", "   ", NULL, &err));
  assert_non_null(err);
  g_clear_error(&err);

  /* 255-char display name truncated in frontmatter title field only (<=200 UTF-8 bytes). */
  gchar long255[256];
  memset(long255, 'Z', 255);
  long255[255] = '\0';
  g_autofree gchar *doc_long = bt_createnote_render_template("bt-20261123-000045", long255,
                                                             "2026-11-23T08:30:00+07:00", &err);
  assert_non_null(doc_long);
  BtFrontmatter *fm_long = bt_frontmatter_parse(doc_long, strlen(doc_long));
  assert_true(fm_long->has_frontmatter);
  assert_non_null(fm_long->title);
  assert_true(strlen(fm_long->title) <= 200u);
  bt_frontmatter_free(fm_long);

  printf("BT_TEST_CT template rendering passed\n");
}

typedef struct {
  const BtVault *vault;
  const gchar *rel;
  const gchar *payload;
} Cw06HookCtx;

static void cw06_inject_before_rename(BtWriteStage stage, void *ud)
{
  if (stage != BT_WRITE_STAGE_BEFORE_RENAME)
    return;
  Cw06HookCtx *ctx = ud;
  g_autofree gchar *abs = vault_resolve(ctx->vault, ctx->rel, NULL);
  if (abs != NULL)
    g_file_set_contents(abs, ctx->payload, -1, NULL);
}

static void crash_at_stage_cb(BtWriteStage stage, void *ud)
{
  BtWriteStage target = (BtWriteStage)GPOINTER_TO_INT(ud);
  if (stage == target) {
    fflush(NULL);
    _exit(99);
  }
}

static gboolean set_dir_readonly(const char *dir, gboolean ro)
{
#ifdef G_OS_WIN32
  gchar *argv_deny[] = {"icacls", (gchar *)dir, "/deny", "*S-1-1-0:(W)", "/Q", NULL};
  gchar *argv_reset[] = {"icacls", (gchar *)dir, "/reset", "/Q", NULL};
  gint ws = 0;
  if (!g_spawn_sync(NULL, ro ? argv_deny : argv_reset, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                    NULL, NULL, NULL, NULL, &ws, NULL))
    return FALSE;
  return g_spawn_check_wait_status(ws, NULL);
#else
  return chmod(dir, ro ? 0555 : 0755) == 0;
#endif
}

static void test_cw01_to_cw14_commit(void **state)
{
  (void)state;
  g_autofree gchar *root = g_dir_make_tmp("bt-l3-cw-XXXXXX", NULL);
  assert_non_null(root);
  bt_test_put(root, "welcome.md", "# Welcome\nSee [[new-idea]]\n");
  BtVault *v = vault_open(root, NULL, NULL);
  assert_non_null(v);
  BtIndexSnapshot *snap = bt_index_snapshot_from_vault(v);

  g_autofree gchar *src_raw = NULL;
  gsize src_len = 0;
  assert_true(vault_read(v, "welcome.md", &src_raw, &src_len, NULL));
  g_autofree gchar *src_sha_before = g_compute_checksum_for_string(G_CHECKSUM_SHA256, src_raw, (gssize)src_len);

  /* CW01 + CW14 timing: Commit CR01 plan */
  BtCreatePlan *p01 = bt_createnote_plan(v, snap, "welcome.md", "new-idea");
  assert_int_equal(p01->result, BT_CREATE_PLAN_OK);
  g_autofree gchar *c_rel = NULL;
  g_autofree gchar *c_id = NULL;
  g_autofree gchar *c_msg = NULL;
  GError *cerr = NULL;
  gint64 t0 = g_get_monotonic_time();
  BtCreateCommitResult cr = bt_createnote_commit(v, p01, &c_rel, &c_id, &c_msg, &cerr);
  double commit_ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
  printf("BT_TEST_CW14 commit_ms=%.3f id=%s\n", commit_ms, c_id ? c_id : "(null)");
  assert_int_equal(cr, BT_CREATE_COMMIT_OK);
  assert_null(cerr);
  assert_string_equal(c_rel, "new-idea.md");
  assert_string_equal(c_msg, "Created new-idea.md");
  assert_true(commit_ms < 500.0);

  /* CW08: LF only, no BOM; CW10: id regex; CW11: key order */
  g_autofree gchar *new_body = NULL;
  gsize new_len = 0;
  assert_true(vault_read(v, "new-idea.md", &new_body, &new_len, NULL));
  assert_null(strstr(new_body, "\r\n"));
  assert_false(new_len >= 3 && (guchar)new_body[0] == 0xEFu && (guchar)new_body[1] == 0xBBu);
  assert_true(g_regex_match_simple("^bt-[0-9]{8}-[0-9]{6}$", c_id, 0, 0));

  BtFrontmatter *fm = bt_frontmatter_parse(new_body, new_len);
  assert_true(fm->has_frontmatter);
  assert_false(fm->parse_error);
  assert_string_equal(fm->id, c_id);
  assert_string_equal(fm->title, "new-idea");
  assert_string_equal(fm->source, "human");
  assert_null(fm->agent);
  g_autofree gchar *exp_same = bt_createnote_render_template(fm->id, "new-idea", fm->created_str, NULL);
  assert_string_equal(new_body, exp_same);
  const char *ordered_keys[] = {"id:", "title:", "created:", "updated:", "source:", "agent:", "tags:", "tags_ai:", "links:"};
  const char *cursor = new_body;
  for (gsize ki = 0; ki < G_N_ELEMENTS(ordered_keys); ki++) {
    const char *hit = strstr(cursor, ordered_keys[ki]);
    assert_non_null(hit);
    cursor = hit + strlen(ordered_keys[ki]);
  }
  bt_frontmatter_free(fm);
  bt_createnote_plan_free(p01);

  /* CW07: Source note untouched */
  g_autofree gchar *src_raw2 = NULL;
  gsize src_len2 = 0;
  assert_true(vault_read(v, "welcome.md", &src_raw2, &src_len2, NULL));
  g_autofree gchar *src_sha_after = g_compute_checksum_for_string(G_CHECKSUM_SHA256, src_raw2, (gssize)src_len2);
  assert_string_equal(src_sha_before, src_sha_after);

  /* CW02: Nested deep folders created atomically */
  BtCreatePlan *p02 = bt_createnote_plan(v, snap, "welcome.md", "brand/new/deep");
  assert_int_equal(p02->result, BT_CREATE_PLAN_OK);
  assert_int_equal(bt_createnote_commit(v, p02, NULL, NULL, NULL, &cerr), BT_CREATE_COMMIT_OK);
  assert_null(cerr);
  g_autofree gchar *deep_abs = vault_resolve(v, "brand/new/deep.md", NULL);
  assert_true(g_file_test(deep_abs, G_FILE_TEST_IS_REGULAR));
  bt_createnote_plan_free(p02);

  /* CW03: Target created on disk after plan -> COMMIT_EXISTS, pre-existing bytes preserved */
  BtCreatePlan *p03 = bt_createnote_plan(v, snap, "welcome.md", "race-target");
  assert_int_equal(p03->result, BT_CREATE_PLAN_OK);
  const char *orig_race = "# Existing untouched\n";
  bt_test_put(root, "race-target.md", orig_race);
  g_autofree gchar *race_msg = NULL;
  assert_int_equal(bt_createnote_commit(v, p03, NULL, NULL, &race_msg, NULL), BT_CREATE_COMMIT_EXISTS);
  assert_string_equal(race_msg, "Note already existed, opened it");
  g_autofree gchar *race_read = NULL;
  gsize race_rlen = 0;
  assert_true(vault_read(v, "race-target.md", &race_read, &race_rlen, NULL));
  assert_string_equal(race_read, orig_race);
  bt_createnote_plan_free(p03);

  /* CW04: Folder segment is an existing file -> COMMIT_BLOCKED */
  bt_test_put(root, "blocking_seg", "not a folder");
  BtCreatePlan *p04 = bt_createnote_plan(v, snap, "welcome.md", "blocking_seg/child");
  assert_int_equal(p04->result, BT_CREATE_PLAN_OK);
  g_autofree gchar *blk_msg = NULL;
  assert_int_equal(bt_createnote_commit(v, p04, NULL, NULL, &blk_msg, &cerr), BT_CREATE_COMMIT_BLOCKED);
  assert_non_null(blk_msg);
  assert_non_null(strstr(blk_msg, "A file named blocking_seg is in the way"));
  g_clear_error(&cerr);
  bt_createnote_plan_free(p04);

  /* CW05: Read-only parent directory */
  {
    g_autofree gchar *ro_dir = g_build_filename(root, "ro_folder", NULL);
    g_mkdir_with_parents(ro_dir, 0755);
    BtCreatePlan *p05 = bt_createnote_plan(v, snap, "welcome.md", "ro_folder/blocked");
    assert_int_equal(p05->result, BT_CREATE_PLAN_OK);
    if (set_dir_readonly(ro_dir, TRUE)) {
      g_autofree gchar *ro_msg = NULL;
      BtCreateCommitResult r05 = bt_createnote_commit(v, p05, NULL, NULL, &ro_msg, &cerr);
      assert_int_equal(r05, BT_CREATE_COMMIT_IO_ERROR);
      assert_non_null(ro_msg);
      assert_true(g_str_has_prefix(ro_msg, "Create failed: "));
      printf("BT_TEST_CW05 ro_msg='%s'\n", ro_msg);
      g_clear_error(&cerr);
      g_autofree gchar *ro_target_abs = g_build_filename(ro_dir, "blocked.md", NULL);
      assert_false(g_file_test(ro_target_abs, G_FILE_TEST_EXISTS));
      set_dir_readonly(ro_dir, FALSE);
    } else {
      printf("BT_TEST_CW05 skipped: OS denied setting read-only dir ACL\n");
    }
    bt_createnote_plan_free(p05);
  }

  /* CW06: Race hook injects destination file between temp sync and create-only publish */
  {
    BtCreatePlan *p06 = bt_createnote_plan(v, snap, "welcome.md", "hook-race");
    assert_int_equal(p06->result, BT_CREATE_PLAN_OK);
    const char *inj = "# Injected during race\n";
    Cw06HookCtx ctx = {v, "hook-race.md", inj};
    BtWriteHooks hooks = {cw06_inject_before_rename, &ctx};
    g_autofree gchar *h_msg = NULL;
    assert_int_equal(bt_createnote_commit_with_hooks(v, p06, &hooks, NULL, NULL, &h_msg, NULL),
                     BT_CREATE_COMMIT_EXISTS);
    assert_string_equal(h_msg, "Note already existed, opened it");
    g_autofree gchar *h_read = NULL;
    gsize h_len = 0;
    assert_true(vault_read(v, "hook-race.md", &h_read, &h_len, NULL));
    assert_string_equal(h_read, inj);
    /* Verify temp file cleaned up. */
    g_autoptr(GDir) gd = g_dir_open(root, 0, NULL);
    const gchar *nm;
    while ((nm = g_dir_read_name(gd)) != NULL)
      assert_false(g_str_has_prefix(nm, ".bt-create-"));
    bt_createnote_plan_free(p06);
  }

  /* CW09: 1,000 consecutive commits produce 1,000 unique note IDs */
  {
    g_autoptr(GHashTable) seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < 1000u; i++) {
      g_autofree gchar *nm = g_strdup_printf("bulk/note_%04u", i);
      BtCreatePlan *pb = bt_createnote_plan(v, snap, "welcome.md", nm);
      assert_int_equal(pb->result, BT_CREATE_PLAN_OK);
      g_autofree gchar *bid = NULL;
      assert_int_equal(bt_createnote_commit(v, pb, NULL, &bid, NULL, &cerr), BT_CREATE_COMMIT_OK);
      assert_null(cerr);
      assert_non_null(bid);
      assert_false(g_hash_table_contains(seen_ids, bid));
      g_hash_table_add(seen_ids, g_steal_pointer(&bid));
      bt_createnote_plan_free(pb);
    }
    assert_int_equal(g_hash_table_size(seen_ids), 1000u);
  }

  /* CW12: Title quote/backslash escaping checked via template + frontmatter round-trip + valid file commit */
  {
    BtCreatePlan *p12 = bt_createnote_plan(v, snap, "welcome.md", "Title 'quoted'");
    assert_int_equal(p12->result, BT_CREATE_PLAN_OK);
    assert_int_equal(bt_createnote_commit(v, p12, NULL, NULL, NULL, &cerr), BT_CREATE_COMMIT_OK);
    bt_createnote_plan_free(p12);
    g_autofree gchar *t12 = bt_createnote_render_template("bt-20261123-000999", "A \"B\" \\ C", NULL, NULL);
    BtFrontmatter *fm12 = bt_frontmatter_parse(t12, strlen(t12));
    assert_string_equal(fm12->title, "A \"B\" \\ C");
    bt_frontmatter_free(fm12);
  }

  /* CW13: Power-loss simulation at every temp write stage via child process */
  if (g_self_exe != NULL) {
    BtWriteStage stages[] = {
      BT_WRITE_STAGE_TMP_OPENED,
      BT_WRITE_STAGE_TMP_PARTIAL,
      BT_WRITE_STAGE_TMP_SYNCED,
      BT_WRITE_STAGE_BEFORE_RENAME,
    };
    for (gsize si = 0; si < G_N_ELEMENTS(stages); si++) {
      g_autofree gchar *cdir = g_dir_make_tmp("bt-cw13-XXXXXX", NULL);
      g_autofree gchar *stage_s = g_strdup_printf("%d", (int)stages[si]);
      gchar *argv[] = {(gchar *)g_self_exe, "--crash-child", cdir, "crash-note", stage_s, NULL};
      gint ws = 0;
      assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &ws, NULL));
      assert_false(g_spawn_check_wait_status(ws, NULL)); /* child aborted as intended */

      g_autofree gchar *dst_check = g_build_filename(cdir, "crash-note.md", NULL);
      assert_false(g_file_test(dst_check, G_FILE_TEST_EXISTS)); /* never partial final file */

      BtScanStats st = {0};
      BtVault *rv = vault_open(cdir, &st, NULL);
      assert_non_null(rv);
      assert_int_equal(st.tmp_removed, 1); /* residue cleaned by vault_open */
      assert_int_equal(vault_notes(rv)->len, 0);
      vault_free(rv);
      if (g_getenv("BT_KEEP_TEST_VAULTS") == NULL)
        bt_test_rm_rf(cdir);
    }
  }

  bt_index_snapshot_free(snap);
  vault_free(v);
  if (g_getenv("BT_KEEP_TEST_VAULTS") == NULL)
    bt_test_rm_rf(root);
  printf("BT_TEST_CW CW01..CW14 passed\n");
}

int main(int argc, char **argv)
{
  setlocale(LC_NUMERIC, "C");
  g_self_exe = (argc > 0) ? argv[0] : NULL;
  if (argc == 5 && strcmp(argv[1], "--crash-child") == 0) {
    BtVault *v = vault_open(argv[2], NULL, NULL);
    if (!v) return 2;
    BtCreatePlan *p = bt_createnote_plan(v, NULL, "welcome.md", argv[3]);
    if (!p || p->result != BT_CREATE_PLAN_OK) return 3;
    BtWriteHooks hooks = {crash_at_stage_cb, GINT_TO_POINTER(atoi(argv[4]))};
    bt_createnote_commit_with_hooks(v, p, &hooks, NULL, NULL, NULL, NULL);
    return 0;
  }
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_cr01_to_cr37_planner),
    cmocka_unit_test(test_cn_name_validation_helpers),
    cmocka_unit_test(test_ct_template_rendering),
    cmocka_unit_test(test_cw01_to_cw14_commit),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
