/* test_noteutil.c — fuzzy matching and wiki-link hit testing. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <glib.h>

#include "noteutil.h"

static void test_fuzzy(void **state)
{
  (void)state;
  assert_int_equal(bt_fuzzy_score("", "x.md"), 0);
  assert_int_equal(bt_fuzzy_score("zz", "abc.md"), -1);
  assert_true(bt_fuzzy_score("cpty", "projects/conpty.md") > 0);
  assert_true(bt_fuzzy_score("CONPTY", "projects/conpty.md") > 0);
  /* basename/word-start matches outrank scattered ones */
  assert_true(bt_fuzzy_score("con", "projects/conpty.md") >
              bt_fuzzy_score("con", "c/o/n/zzz.md"));
  assert_true(bt_fuzzy_score("daily", "daily.md") > bt_fuzzy_score("daily", "d-a-i-l-y-long-name.md"));
  assert_int_equal(bt_fuzzy_score(NULL, "x"), -1);
}

static void check_link(const gchar *line, gsize off, const gchar *want)
{
  gchar *got = bt_wikilink_at(line, off);
  if (want == NULL)
    assert_null(got);
  else
    assert_string_equal(got, want);
  g_free(got);
}

static void test_wikilink_at(void **state)
{
  (void)state;
  const gchar *line = "see [[Win32 API]] and [[notes/ConPTY|alias]] or [[X#Heading]].";
  check_link(line, 0, NULL);
  check_link(line, 4, "Win32 API");  /* on the first '[' */
  check_link(line, 10, "Win32 API");
  check_link(line, 16, "Win32 API"); /* on the last ']' */
  check_link(line, 17, NULL);
  check_link(line, 25, "notes/ConPTY");
  check_link(line, 50, "X");
  check_link("[[]]", 1, NULL);
  check_link("[[  ]]", 2, NULL);
  check_link("[[unclosed", 3, NULL);
  check_link("[[a [[b]]", 1, NULL);
  check_link(NULL, 0, NULL);
}

static void test_wikilink_resolve(void **state)
{
  (void)state;
  GPtrArray *notes = g_ptr_array_new();
  g_ptr_array_add(notes, "inbox/Win32 API.md");
  g_ptr_array_add(notes, "projects/conpty.md");
  g_ptr_array_add(notes, "conpty.md");
  gchar *r = bt_wikilink_find_note(notes, "win32 api");
  assert_string_equal(r, "inbox/Win32 API.md");
  g_free(r);
  r = bt_wikilink_find_note(notes, "projects/ConPTY");
  assert_string_equal(r, "projects/conpty.md");
  g_free(r);
  r = bt_wikilink_find_note(notes, "conpty"); /* exact relpath beats basename */
  assert_string_equal(r, "conpty.md");
  g_free(r);
  assert_null(bt_wikilink_find_note(notes, "missing"));
  g_ptr_array_unref(notes);

  gchar *p = bt_wikilink_new_note_path("sub\\New Note");
  assert_string_equal(p, "sub/New Note.md");
  g_free(p);
  p = bt_wikilink_new_note_path("x.MD");
  assert_string_equal(p, "x.MD");
  g_free(p);
  assert_null(bt_wikilink_new_note_path(""));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_fuzzy),
    cmocka_unit_test(test_wikilink_at),
    cmocka_unit_test(test_wikilink_resolve),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
