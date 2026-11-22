/* test_mdparse.c — MD4C extraction: headings, wiki-links (never in code fences
 * or inline code), #tags, plain text (§11.1). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "mdparse.h"

static void test_mdparse_corpus(void **state)
{
  (void)state;
  const gchar *md =
      "# First Heading\n\n"
      "See [[Win32 API]] and [[ConPTY|alias]] #windows #C_lang.\n\n"
      "Inline code `[[IgnoreInline]] #ignore_inline` should not extract.\n\n"
      "```c\n"
      "// [[IgnoreFence]] #ignore_fence\n"
      "int x = 1;\n"
      "```\n\n"
      "After fence [[RealLink#sec]].\n";

  BtMdExtract *ex = bt_mdparse_extract(md, strlen(md));
  assert_string_equal(ex->first_h1, "First Heading");

  assert_int_equal(ex->wikilinks->len, 3);
  assert_string_equal(g_ptr_array_index(ex->wikilinks, 0), "Win32 API");
  assert_string_equal(g_ptr_array_index(ex->wikilinks, 1), "ConPTY");
  assert_string_equal(g_ptr_array_index(ex->wikilinks, 2), "RealLink");

  assert_int_equal(ex->tags->len, 2);
  assert_string_equal(g_ptr_array_index(ex->tags, 0), "windows");
  assert_string_equal(g_ptr_array_index(ex->tags, 1), "c_lang");

  assert_non_null(strstr(ex->plain_body, "First Heading"));
  bt_mdparse_free(ex);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_mdparse_corpus),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
