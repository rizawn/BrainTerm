/* test_linkresolve.c — Groups R (R01..R38), P (parser), and U (security) for Phase L1. */
#ifndef G_OS_WIN32
#define _POSIX_C_SOURCE 200809L
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>

#include "linkresolve.h"
#include "testutil.h"
#include "vault.h"

#ifdef G_OS_WIN32
static gboolean create_dir_junction_or_symlink(const char *link_path, const char *target_dir)
{
  /* Escape paths via cmd mklink /J (directory junction requires no elevation on Win 10/11). */
  g_autofree wchar_t *wl = g_utf8_to_utf16(link_path, -1, NULL, NULL, NULL);
  g_autofree wchar_t *wt = g_utf8_to_utf16(target_dir, -1, NULL, NULL, NULL);
  g_autofree gchar *cmd = g_strdup_printf("cmd.exe /c mklink /J \"%s\" \"%s\" >NUL 2>&1", link_path, target_dir);
  (void)wl;
  (void)wt;
  return system(cmd) == 0;
}
#else
#include <unistd.h>
static gboolean create_dir_junction_or_symlink(const char *link_path, const char *target_dir)
{
  return symlink(target_dir, link_path) == 0;
}
#endif

static BtIndexSnapshot *build_fixture_snapshot(void)
{
  BtIndexSnapshot *snap = bt_index_snapshot_new();
  static const char *const conpty_aliases[] = { "ConPTY Intro", NULL };
  static const char *const boot_aliases[] = { "Bootstrap Notes", "Resampling", NULL };

  bt_index_snapshot_add_entry(snap, "welcome.md", FALSE, "Home", NULL);
  bt_index_snapshot_add_entry(snap, "my note.md", FALSE, "My Note", NULL);
  bt_index_snapshot_add_entry(snap, "projects", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "projects/conpty.md", FALSE, "ConPTY Notes", conpty_aliases);
  bt_index_snapshot_add_entry(snap, "projects/notes", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "projects/notes/conpty.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "statistical-statistics-notes", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "statistical-statistics-notes/MathStats.md", FALSE, "MathStats", NULL);
  bt_index_snapshot_add_entry(snap, "statistical-statistics-notes/chapter-01-descriptive", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "statistical-statistics-notes/chapter-01-descriptive/01-data-types-collection.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "statistical-statistics-notes/chapter-13-empirical", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", FALSE, NULL, boot_aliases);
  bt_index_snapshot_add_entry(snap, "Daily", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "Daily/2026-11-23.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "R\xC3\xA9sum\xC3\xA9.md", FALSE, NULL, NULL); /* NFC: Résumé.md */
  bt_index_snapshot_add_entry(snap, "assets", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "assets/diagram.png", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "assets/paper.pdf", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "scripts", TRUE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "scripts/run.ps1", FALSE, NULL, NULL);

  bt_index_snapshot_finalize(snap);
  return snap;
}

typedef struct {
  const char *id;
  const char *source;
  const char *target;
  gboolean expect_null;
  BtLinkStatus status;
  BtLinkKind kind;
  BtLinkBlockReason reason;
  const char *path;
  const char *anchor;
  const char *block_id;
  const char *display;
  const char *url;
} ResolverRow;

static void test_group_r_resolver_table(void **state)
{
  (void)state;
  BtIndexSnapshot *snap = build_fixture_snapshot();

  gchar long_target[1105];
  memset(long_target, 'a', 1100);
  long_target[1100] = '\0';

  const ResolverRow rows[] = {
    { "R01", "welcome.md", "welcome", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "welcome.md", NULL, NULL, "welcome", NULL },
    { "R03", "welcome.md", "welcome.md", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "welcome.md", NULL, NULL, "welcome.md", NULL },
    { "R04", "statistical-statistics-notes/MathStats.md", "chapter-01-descriptive/01-data-types-collection.md", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-01-descriptive/01-data-types-collection.md", NULL, NULL, NULL, NULL },
    { "R05", "welcome.md", "statistical-statistics-notes/chapter-13-empirical/02-bootstrap", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", NULL, NULL, NULL, NULL },
    { "R06", "welcome.md", "02-bootstrap", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", NULL, NULL, NULL, NULL },
    { "R07", "welcome.md", "conpty", FALSE, BT_LINK_AMBIGUOUS, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "projects/conpty.md", NULL, NULL, NULL, NULL },
    { "R08", "projects/conpty.md", "conpty", FALSE, BT_LINK_AMBIGUOUS, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "projects/conpty.md", NULL, NULL, NULL, NULL },
    { "R09", "welcome.md", "projects/conpty", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "projects/conpty.md", NULL, NULL, NULL, NULL },
    { "R10", "welcome.md", "missing-note", FALSE, BT_LINK_UNRESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, NULL, NULL, NULL, NULL, NULL },
    { "R11", "welcome.md", "02-bootstrap#Overview", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", "Overview", NULL, NULL, NULL },
    { "R12", "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", "#Core Concepts", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_SAME_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", "Core Concepts", NULL, "Core Concepts", NULL },
    { "R13", "welcome.md", "02-bootstrap|Bootstrap", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", NULL, NULL, "Bootstrap", NULL },
    { "R14", "welcome.md", "02-bootstrap\\|Bootstrap", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", NULL, NULL, "Bootstrap", NULL },
    { "R15", "welcome.md", "02-bootstrap^abc123", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", NULL, "abc123", NULL, NULL },
    { "R16", "welcome.md", "../outside", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_ESCAPES_VAULT, NULL, NULL, NULL, NULL, NULL },
    { "R17", "projects/conpty.md", "../welcome", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "welcome.md", NULL, NULL, NULL, NULL },
    { "R18", "welcome.md", "assets/paper.pdf", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_FILE, BT_LINK_BLOCK_NONE, "assets/paper.pdf", NULL, NULL, NULL, NULL },
    { "R19", "welcome.md", "scripts/run.ps1", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_FILE, BT_LINK_BLOCK_NONE, "scripts/run.ps1", NULL, NULL, NULL, NULL },
    { "R20", "welcome.md", "assets", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_DIR, BT_LINK_BLOCK_NONE, "assets", NULL, NULL, NULL, NULL },
    { "R21", "welcome.md", "Re\xCC\x81sum\x65\xCC\x81", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "R\xC3\xA9sum\xC3\xA9.md", NULL, NULL, NULL, NULL }, /* NFD -> NFC */
    { "R22", "welcome.md", "C:\\Windows\\system32\\calc.exe", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_FILE, BT_LINK_BLOCK_UNSAFE_PATH, NULL, NULL, NULL, NULL, NULL },
    { "R23", "welcome.md", "\\\\server\\share\\a.md", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_UNSAFE_PATH, NULL, NULL, NULL, NULL, NULL },
    { "R24", "welcome.md", "a.md:stream", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_FILE, BT_LINK_BLOCK_UNSAFE_PATH, NULL, NULL, NULL, NULL, NULL },
    { "R25", "welcome.md", "NUL", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_FILE, BT_LINK_BLOCK_UNSAFE_PATH, NULL, NULL, NULL, NULL, NULL },
    { "R26", "welcome.md", "name. ", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_FILE, BT_LINK_BLOCK_UNSAFE_PATH, NULL, NULL, NULL, NULL, NULL },
    { "R27", "welcome.md", "https://example.org/x", FALSE, BT_LINK_EXTERNAL, BT_LINK_KIND_URL, BT_LINK_BLOCK_NONE, NULL, NULL, NULL, NULL, "https://example.org/x" },
    { "R28", "welcome.md", "javascript:alert(1)", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_URL, BT_LINK_BLOCK_SCHEME_NOT_ALLOWED, NULL, NULL, NULL, NULL, NULL },
    { "R29", "welcome.md", "file:///C:/x", FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_URL, BT_LINK_BLOCK_SCHEME_NOT_ALLOWED, NULL, NULL, NULL, NULL, NULL },
    { "R30", "welcome.md", "mailto:a@b.c", FALSE, BT_LINK_EXTERNAL, BT_LINK_KIND_URL, BT_LINK_BLOCK_NONE, NULL, NULL, NULL, NULL, "mailto:a@b.c" },
    { "R31", "welcome.md", "", TRUE, 0, 0, 0, NULL, NULL, NULL, NULL, NULL },
    { "R32", "welcome.md", long_target, FALSE, BT_LINK_BLOCKED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_TOO_LONG, NULL, NULL, NULL, NULL, NULL },
    { "R33", "welcome.md", "my%20note.md", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "my note.md", NULL, NULL, NULL, NULL },
    { "R34", "welcome.md", "projects\\conpty.md", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "projects/conpty.md", NULL, NULL, NULL, NULL },
    { "R35", "welcome.md", "projects//conpty.md", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "projects/conpty.md", NULL, NULL, NULL, NULL },
    { "R36", "welcome.md", "ConPTY Notes", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "projects/conpty.md", NULL, NULL, NULL, NULL },
    { "R37", "welcome.md", "Bootstrap Notes", FALSE, BT_LINK_RESOLVED, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, "statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md", NULL, NULL, NULL, NULL },
    { "P3A", "welcome.md", "{{source-1}}", FALSE, BT_LINK_PLACEHOLDER, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, NULL, NULL, NULL, "{{source-1}}", NULL },
    { "P3B", "welcome.md", "{{related-note}}|Related", FALSE, BT_LINK_PLACEHOLDER, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, NULL, NULL, NULL, "Related", NULL },
    { "P3C", "welcome.md", "{{}}", FALSE, BT_LINK_PLACEHOLDER, BT_LINK_KIND_NOTE, BT_LINK_BLOCK_NONE, NULL, NULL, NULL, "{{}}", NULL },
  };

  for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
    const ResolverRow *rw = &rows[i];
    BtLinkResolution *r = bt_link_resolve(snap, rw->source, rw->target);
    if (rw->expect_null) {
      if (r != NULL)
        fail_msg("%s expected NULL", rw->id);
      continue;
    }
    if (r == NULL)
      fail_msg("%s returned NULL unexpectedly", rw->id);
    if (r->status != rw->status || r->kind != rw->kind || r->block_reason != rw->reason) {
      fail_msg("%s mismatch: got status=%d kind=%d reason=%d, expected %d/%d/%d",
               rw->id, r->status, r->kind, r->block_reason, rw->status, rw->kind, rw->reason);
    }
    if (rw->path)
      assert_string_equal(r->path, rw->path);
    if (rw->anchor)
      assert_string_equal(r->anchor, rw->anchor);
    if (rw->block_id)
      assert_string_equal(r->block_id, rw->block_id);
    if (rw->display)
      assert_string_equal(r->display, rw->display);
    if (rw->url)
      assert_string_equal(r->url, rw->url);
    bt_link_resolution_free(r);
  }

  /* Verify R07 & R08 ambiguity candidate ordering explicitly. */
  BtLinkResolution *r07 = bt_link_resolve(snap, "welcome.md", "conpty");
  assert_non_null(r07->candidates);
  assert_string_equal(r07->candidates[0], "projects/conpty.md");
  assert_string_equal(r07->candidates[1], "projects/notes/conpty.md");
  bt_link_resolution_free(r07);

  BtLinkResolution *r08 = bt_link_resolve(snap, "projects/conpty.md", "conpty");
  assert_int_equal(r08->status, BT_LINK_AMBIGUOUS);
  assert_string_equal(r08->candidates[0], "projects/conpty.md");
  assert_string_equal(r08->candidates[1], "projects/notes/conpty.md");
  bt_link_resolution_free(r08);

  BtLinkResolution *r08_nested = bt_link_resolve(snap, "projects/notes/conpty.md", "conpty");
  assert_int_equal(r08_nested->status, BT_LINK_AMBIGUOUS);
  assert_string_equal(r08_nested->candidates[0], "projects/notes/conpty.md");
  assert_string_equal(r08_nested->candidates[1], "projects/conpty.md");
  bt_link_resolution_free(r08_nested);

  /* P-4: source-subtree and non-hidden dot-folder preference over shallower .raw/. */
  BtIndexSnapshot *p4_snap = bt_index_snapshot_new();
  bt_index_snapshot_add_entry(p4_snap, "claude-obsidian/wiki/index.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(p4_snap, "claude-obsidian/.raw/claude-obsidian-ecosystem-research.md", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(p4_snap, "claude-obsidian/wiki/sources/claude-obsidian-ecosystem-research.md", FALSE, NULL, NULL);
  bt_index_snapshot_finalize(p4_snap);
  BtLinkResolution *p4_res = bt_link_resolve(p4_snap, "claude-obsidian/wiki/index.md", "claude-obsidian-ecosystem-research");
  assert_non_null(p4_res);
  assert_int_equal(p4_res->status, BT_LINK_AMBIGUOUS);
  assert_string_equal(p4_res->candidates[0], "claude-obsidian/wiki/sources/claude-obsidian-ecosystem-research.md");
  assert_string_equal(p4_res->candidates[1], "claude-obsidian/.raw/claude-obsidian-ecosystem-research.md");
  bt_link_resolution_free(p4_res);
  bt_index_snapshot_free(p4_snap);

  /* Snapshot build timing for 3,000 notes (< 20 ms target, < 60 ms contention-safe CI guard; ADR-023). */
  BtIndexSnapshot *s3k = bt_index_snapshot_new();
  gint64 s0 = g_get_monotonic_time();
  for (guint k = 0; k < 3000; k++) {
    g_autofree gchar *p = g_strdup_printf("folder_%u/note_%u.md", k % 20u, k);
    g_autofree gchar *t = g_strdup_printf("Note Title %u", k);
    bt_index_snapshot_add_entry(s3k, p, FALSE, t, NULL);
  }
  bt_index_snapshot_finalize(s3k);
  double s3k_ms = (double)(g_get_monotonic_time() - s0) / 1000.0;
  printf("BT_TEST_LINK_SNAP 3k_notes build_ms=%.3f target_ms=20.0 ci_ceiling_ms=60.0\n", s3k_ms);
  assert_true(s3k_ms < 60.0);
  bt_index_snapshot_free(s3k);

  /* R02: Case-insensitive on Windows, case-sensitive on Linux. */
  bt_index_snapshot_set_case_sensitive(snap, FALSE);
  bt_index_snapshot_finalize(snap);
  BtLinkResolution *r02_win = bt_link_resolve(snap, "welcome.md", "Welcome");
  assert_int_equal(r02_win->status, BT_LINK_RESOLVED);
  bt_link_resolution_free(r02_win);

  bt_index_snapshot_set_case_sensitive(snap, TRUE);
  bt_index_snapshot_finalize(snap);
  BtLinkResolution *r02_lin = bt_link_resolve(snap, "welcome.md", "Welcome");
  assert_int_equal(r02_lin->status, BT_LINK_UNRESOLVED);
  bt_link_resolution_free(r02_lin);

  bt_index_snapshot_free(snap);

  /* R38: Symlink / junction pointing outside vault must be BLOCKED ESCAPES_VAULT. */
  gchar *parent = g_dir_make_tmp("bt-r38-XXXXXX", NULL);
  gchar *vdir = g_build_filename(parent, "vault", NULL);
  gchar *out_dir = g_build_filename(parent, "outside", NULL);
  g_mkdir_with_parents(vdir, 0755);
  g_mkdir_with_parents(out_dir, 0755);
  bt_test_put(vdir, "welcome.md", "# Welcome\n");
  bt_test_put(out_dir, "secret.md", "# Top Secret Outside\n");
  gchar *junc = g_build_filename(vdir, "escape_dir", NULL);
  gboolean linked = create_dir_junction_or_symlink(junc, out_dir);
  if (linked) {
    BtVault *v = vault_open(vdir, NULL, NULL);
    BtIndexSnapshot *vsnap = bt_index_snapshot_from_vault(v);
    BtLinkResolution *r38 = bt_link_resolve(vsnap, "welcome.md", "escape_dir/secret.md");
    assert_non_null(r38);
    assert_int_equal(r38->status, BT_LINK_BLOCKED);
    assert_int_equal(r38->block_reason, BT_LINK_BLOCK_ESCAPES_VAULT);
    bt_link_resolution_free(r38);
    bt_index_snapshot_free(vsnap);
    vault_free(v);
    g_rmdir(junc);
  }
  g_free(junc);
  g_free(out_dir);
  g_free(vdir);
  bt_test_rm_rf(parent);
  g_free(parent);
}

static void test_group_p_parser(void **state)
{
  (void)state;
  const char *doc =
      "\xEF\xBB\xBF---\r\n"
      "title: \"Parser\"\r\n"
      "links: [\"[[welcome]]\"]\r\n"
      "---\r\n"
      "# Heading [[in_h1]]\r\n"
      "- Item [[in_list]] and [[adj1]][[adj2]]\r\n"
      "> Quote [[in_quote]]\r\n"
      "| Col1 | [[in_table]] | [[a\\|b]] |\r\n"
      "`[[not_inline_code]]` and $[[not_math_link]]$ and `[[{{code_placeholder}}]]`\r\n"
      "```\r\n[[not_fenced]]\r\n[[{{fenced_placeholder}}]]\r\nhttps://not.in.code\r\n```\r\n"
      "Template placeholder [[{{source-1}}]] outside code.\r\n"
      "Nested [[a [b] c]] ignored.\r\n"
      "[Wiki](https://en.wikipedia.org/wiki/Bootstrap_(statistics)) and [sp](<my note.md>)\r\n"
      "See https://example.org/page_(x), or https://example.org/end.\r\n"
      "Unicode [[\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xF0\x9F\xA7\xA0]].\r\n";

  GArray *sp = bt_link_scan(doc, strlen(doc));
  const char *exp[] = {
    "welcome",
    "in_h1",
    "in_list",
    "adj1",
    "adj2",
    "in_quote",
    "in_table",
    "a\\|b",
    "{{source-1}}",
    "https://en.wikipedia.org/wiki/Bootstrap_(statistics)",
    "my note.md",
    "https://example.org/page_(x)",
    "https://example.org/end",
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xF0\x9F\xA7\xA0"
  };
  assert_int_equal(sp->len, G_N_ELEMENTS(exp));
  for (guint i = 0; i < sp->len; i++) {
    const BtLinkSpan *s = &g_array_index(sp, BtLinkSpan, i);
    assert_string_equal(s->raw_target, exp[i]);
  }
  g_array_unref(sp);

  /* 10,000 links in one note under 50 ms (§A.16 Group P). */
  GString *big = g_string_sized_new(260000);
  for (int i = 0; i < 10000; i++)
    g_string_append_printf(big, "Link [[note_%d]] here.\n", i);
  gint64 t0 = g_get_monotonic_time();
  GArray *big_sp = bt_link_scan(big->str, big->len);
  double ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
  printf("BT_TEST_LINK_SCAN 10k_links count=%u elapsed_ms=%.3f\n", big_sp->len, ms);
  assert_int_equal(big_sp->len, 10000);
  assert_true(ms < 50.0);
  g_array_unref(big_sp);
  g_string_free(big, TRUE);

  /* Seeded incremental property test: scan_range == full scan for touched block. */
  GString *pdoc = g_string_new(
      "Para 1 [[welcome]] and https://example.org\n\n"
      "Para 2 [doc](projects/conpty.md)\n\n"
      "```sh\n[[skip]]\n```\n\n"
      "Para 3 [[02-bootstrap|Boot]] end.\n");
  GRand *rng = g_rand_new_with_seed(0x4C313031u);
  const char *snips[] = { " [[x]] ", " [t](a.md) ", "\n\n", "$[[no]]$", " text ", " https://a.b/c " };
  for (int it = 0; it < 150; it++) {
    gsize pos = pdoc->len ? (gsize)g_rand_int_range(rng, 0, (gint32)pdoc->len) : 0;
    const char *sn = snips[g_rand_int_range(rng, 0, G_N_ELEMENTS(snips))];
    g_string_insert(pdoc, pos, sn);
    gsize bs = 0, be = 0;
    GArray *part = bt_link_scan_range(pdoc->str, pdoc->len, pos, pos + strlen(sn), &bs, &be);
    GArray *full = bt_link_scan(pdoc->str, pdoc->len);
    guint k = 0;
    for (guint i = 0; i < full->len; i++) {
      const BtLinkSpan *fs = &g_array_index(full, BtLinkSpan, i);
      if (fs->start >= bs && fs->end <= be) {
        assert_true(k < part->len);
        const BtLinkSpan *ps = &g_array_index(part, BtLinkSpan, k);
        assert_int_equal(ps->start, fs->start);
        assert_int_equal(ps->end, fs->end);
        assert_string_equal(ps->raw_target, fs->raw_target);
        k++;
      }
    }
    assert_int_equal(k, part->len);
    g_array_unref(part);
    g_array_unref(full);
  }
  g_rand_free(rng);
  g_string_free(pdoc, TRUE);
}

static gboolean mock_launcher(const char *target, gboolean is_uri, void *ud)
{
  (void)target;
  (void)is_uri;
  guint *calls = ud;
  (*calls)++;
  return TRUE;
}

static void test_group_u_security(void **state)
{
  (void)state;
  const char *blocked_exts[] = {
    "exe", "com", "bat", "cmd", "ps1", "psm1", "psd1", "vbs", "vbe", "js", "jse",
    "wsf", "wsh", "msi", "msp", "scr", "pif", "lnk", "url", "reg", "hta", "cpl",
    "msc", "jar", "dll", "sys", "inf", "appx", "msix", "ps1xml", "gadget", "application"
  };

  guint mock_calls = 0;
  BtIndexSnapshot *snap = bt_index_snapshot_new();

  /* Add every blocked ext with mixed case, trailing dot/space, and double ext. */
  for (gsize i = 0; i < G_N_ELEMENTS(blocked_exts); i++) {
    g_autofree gchar *p1 = g_strdup_printf("f_%zu.%s", i, blocked_exts[i]);
    g_autofree gchar *up = g_ascii_strup(blocked_exts[i], -1);
    g_autofree gchar *p2 = g_strdup_printf("d_%zu.pdf.%s", i, up);
    bt_index_snapshot_add_entry(snap, p1, FALSE, NULL, NULL);
    bt_index_snapshot_add_entry(snap, p2, FALSE, NULL, NULL);

    BtLinkResolution *r1 = bt_link_resolve(snap, "welcome.md", p1);
    BtLinkOpenPolicy pol1;
    assert_false(bt_link_dispatch_open(r1, NULL, TRUE, FALSE, mock_launcher, &mock_calls, &pol1));
    assert_int_equal(pol1.action, BT_LINK_OPEN_BLOCKED);
    bt_link_open_policy_clear(&pol1);
    bt_link_resolution_free(r1);

    BtLinkResolution *r2 = bt_link_resolve(snap, "welcome.md", p2);
    BtLinkOpenPolicy pol2;
    assert_false(bt_link_dispatch_open(r2, NULL, TRUE, FALSE, mock_launcher, &mock_calls, &pol2));
    assert_int_equal(pol2.action, BT_LINK_OPEN_BLOCKED);
    bt_link_open_policy_clear(&pol2);
    bt_link_resolution_free(r2);
  }
  assert_int_equal(mock_calls, 0);

  /* Appendix 2 hostile list: ALL 14 samples MUST have 0 launcher calls (without confirm). */
  const char *hostile[] = {
    "..\\..\\Windows\\System32\\calc.exe",
    "C:\\Windows\\System32\\cmd.exe",
    "\\\\evil\\share\\payload.exe",
    "assets/paper.pdf.exe",
    "scripts/run.ps1",
    "javascript:alert(1)",
    "file:///C:/Windows/System32/calc.exe",
    "ms-msdt:/id PCWDiagnostic",
    "search-ms:query=a",
    "https://user:pass@example.org/",
    "data:text/html;base64,PHNjcmlwdD4=",
    "CON",
    "a.txt:hidden",
    "note.md ",
  };
  bt_index_snapshot_add_entry(snap, "assets/paper.pdf.exe", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "scripts/run.ps1", FALSE, NULL, NULL);
  bt_index_snapshot_finalize(snap);

  for (gsize i = 0; i < G_N_ELEMENTS(hostile); i++) {
    BtLinkResolution *r = bt_link_resolve(snap, "welcome.md", hostile[i]);
    BtLinkOpenPolicy pol;
    gboolean launched = bt_link_dispatch_open(r, NULL, FALSE, TRUE, mock_launcher, &mock_calls, &pol);
    assert_false(launched);
    if (strcmp(hostile[i], "https://user:pass@example.org/") == 0) {
      assert_int_equal(pol.action, BT_LINK_OPEN_CONFIRM_URL);
      assert_true(pol.has_userinfo);
      assert_string_equal(pol.safe_display_url, "https://example.org/");
    } else {
      assert_int_equal(pol.action, BT_LINK_OPEN_BLOCKED);
    }
    bt_link_open_policy_clear(&pol);
    bt_link_resolution_free(r);
  }
  assert_int_equal(mock_calls, 0);

  /* Allow-listed extension opens without confirm; unknown extension requires confirm. */
  bt_index_snapshot_add_entry(snap, "assets/paper.pdf", FALSE, NULL, NULL);
  bt_index_snapshot_add_entry(snap, "assets/archive.xyz", FALSE, NULL, NULL);
  bt_index_snapshot_finalize(snap);

  BtLinkResolution *r_pdf = bt_link_resolve(snap, "welcome.md", "assets/paper.pdf");
  BtLinkOpenPolicy p_pdf;
  assert_true(bt_link_dispatch_open(r_pdf, NULL, FALSE, TRUE, mock_launcher, &mock_calls, &p_pdf));
  assert_int_equal(p_pdf.action, BT_LINK_OPEN_ALLOW);
  assert_int_equal(mock_calls, 1);
  bt_link_open_policy_clear(&p_pdf);
  bt_link_resolution_free(r_pdf);

  BtLinkResolution *r_xyz = bt_link_resolve(snap, "welcome.md", "assets/archive.xyz");
  BtLinkOpenPolicy p_xyz;
  assert_false(bt_link_dispatch_open(r_xyz, NULL, FALSE, TRUE, mock_launcher, &mock_calls, &p_xyz));
  assert_int_equal(p_xyz.action, BT_LINK_OPEN_CONFIRM_EXT);
  assert_int_equal(mock_calls, 1);
  assert_true(bt_link_dispatch_open(r_xyz, NULL, TRUE, TRUE, mock_launcher, &mock_calls, &p_xyz));
  bt_link_open_policy_clear(&p_xyz);
  assert_int_equal(mock_calls, 2);
  bt_link_resolution_free(r_xyz);

  printf("BT_TEST_LINK_SECURITY blocked_exts=%zu hostile=%zu mock_calls_after_allow_and_confirm=%u\n",
         G_N_ELEMENTS(blocked_exts), G_N_ELEMENTS(hostile), mock_calls);
  bt_index_snapshot_free(snap);
}

int main(void)
{
  setlocale(LC_NUMERIC, "C");
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_group_r_resolver_table),
    cmocka_unit_test(test_group_p_parser),
    cmocka_unit_test(test_group_u_security),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
