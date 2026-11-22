/* test_frontmatter.c — §7.1 libyaml frontmatter corpus (valid, unknown keys,
 * duplicate keys -> parse_error, round-trip serialize). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "frontmatter.h"

static void test_valid_and_unknown_preserved(void **state)
{
  (void)state;
  const gchar *doc =
      "---\n"
      "id: bt-20261122-a1b2c3\n"
      "title: \"Learning ConPTY\"\n"
      "created: 2026-11-22T14:00:00+07:00\n"
      "updated: 2026-11-22T14:30:00+07:00\n"
      "source: human\n"
      "agent: null\n"
      "tags: [terminal, windows, c]\n"
      "tags_ai: [pty]\n"
      "links: [\"[[Win32 API]]\"]\n"
      "custom_meta: keep-me-verbatim\n"
      "---\n"
      "# Body\n";

  BtFrontmatter *fm = bt_frontmatter_parse(doc, strlen(doc));
  assert_true(fm->has_frontmatter);
  assert_false(fm->parse_error);
  assert_string_equal(fm->id, "bt-20261122-a1b2c3");
  assert_string_equal(fm->title, "Learning ConPTY");
  assert_string_equal(fm->source, "human");
  assert_null(fm->agent);
  assert_true(fm->created_ms > 0);
  assert_int_equal(fm->tags->len, 3);
  assert_int_equal(fm->tags_ai->len, 1);
  assert_int_equal(fm->links->len, 1);
  assert_int_equal(fm->unknown->len, 1);
  assert_string_equal(doc + fm->body_offset, "# Body\n");

  gchar *ser = bt_frontmatter_serialize(fm, doc + fm->body_offset);
  assert_non_null(strstr(ser, "custom_meta: keep-me-verbatim"));
  assert_non_null(strstr(ser, "tags: [terminal, windows, c]"));
  assert_non_null(strstr(ser, "# Body\n"));

  /* Re-parse serialized output -> identical fields. */
  BtFrontmatter *fm2 = bt_frontmatter_parse(ser, strlen(ser));
  assert_false(fm2->parse_error);
  assert_string_equal(fm2->id, fm->id);
  assert_string_equal(fm2->title, fm->title);
  assert_int_equal(fm2->unknown->len, 1);

  bt_frontmatter_free(fm2);
  g_free(ser);
  bt_frontmatter_free(fm);
}

static void test_duplicate_and_malformed(void **state)
{
  (void)state;
  const gchar *dup =
      "---\n"
      "title: first\n"
      "title: second\n"
      "---\nbody\n";
  BtFrontmatter *fm = bt_frontmatter_parse(dup, strlen(dup));
  assert_true(fm->parse_error);
  bt_frontmatter_free(fm);

  const gchar *bad = "---\n: [unclosed\n---\nbody\n";
  fm = bt_frontmatter_parse(bad, strlen(bad));
  assert_true(fm->parse_error);
  bt_frontmatter_free(fm);

  const gchar *plain = "# No frontmatter\nhello\n";
  fm = bt_frontmatter_parse(plain, strlen(plain));
  assert_false(fm->has_frontmatter);
  assert_false(fm->parse_error);
  assert_int_equal(fm->body_offset, 0);
  bt_frontmatter_free(fm);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_valid_and_unknown_preserved),
    cmocka_unit_test(test_duplicate_and_malformed),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
