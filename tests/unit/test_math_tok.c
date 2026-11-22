/* test_math_tok.c — Group T math tokenizer tests (Phase M1, §B.2 & §B.21). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "math/mathtok.h"

static gchar *span_src(const char *doc, const BtMathSpan *sp)
{
  return g_strndup(doc + sp->src_start, sp->src_end - sp->src_start);
}

static void test_pandoc_dollar_rules(void **state)
{
  (void)state;
  const char *s1 = "$x$";
  GArray *a = bt_math_scan(s1, strlen(s1), NULL);
  assert_int_equal(a->len, 1);
  BtMathSpan *sp = &g_array_index(a, BtMathSpan, 0);
  assert_int_equal(sp->mode, BT_MATH_INLINE);
  assert_int_equal(sp->start, 0);
  assert_int_equal(sp->end, 3);
  g_autofree gchar *src1 = span_src(s1, sp);
  assert_string_equal(src1, "x");
  g_array_unref(a);

  const char *negatives[] = {
    "$ x$",
    "$x $",
    "$x$1",
    "\\$5",
    "It costs $5 and $10 in total.",
    "$not math $",
    "unterminated $x + y",
    "unterminated $$\n\\alpha + \\beta\n",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(negatives); i++) {
    GArray *r = bt_math_scan(negatives[i], strlen(negatives[i]), NULL);
    assert_int_equal(r->len, 0);
    g_array_unref(r);
  }

  const char *mixed = "It costs $5 and $10, and $x$ and $y$ work, plus $a + \\$ + b$.";
  GArray *m = bt_math_scan(mixed, strlen(mixed), NULL);
  assert_int_equal(m->len, 3);
  g_autofree gchar *m0 = span_src(mixed, &g_array_index(m, BtMathSpan, 0));
  g_autofree gchar *m1 = span_src(mixed, &g_array_index(m, BtMathSpan, 1));
  g_autofree gchar *m2 = span_src(mixed, &g_array_index(m, BtMathSpan, 2));
  assert_string_equal(m0, "x");
  assert_string_equal(m1, "y");
  assert_string_equal(m2, "a + \\$ + b");
  g_array_unref(m);
}

static void test_display_and_contexts(void **state)
{
  (void)state;
  const char *doc =
      "\xEF\xBB\xBF---\r\n"
      "title: \"$not_fm$\"\r\n"
      "---\r\n"
      "# Heading with $h_1$\r\n\r\n"
      "> $$q_1$$\r\n\r\n"
      "1. Item with $i_1$ and `inline $not_code$`\r\n"
      "| $a | b$ | [[link $not_wl$]] | [label $in_txt$](https://example.org/$not_url$) |\r\n"
      "<!-- comment $not_html$ -->\r\n"
      "```python\r\nx = \"$not_fence$\"\r\n```\r\n"
      "Also https://example.org/path/$not_bare$ and multiline:\r\n"
      "$$\r\n"
      "\\sum_{i=1}^n x_i\r\n"
      "$$\r\n";

  GArray *spans = bt_math_scan(doc, strlen(doc), NULL);
  assert_int_equal(spans->len, 6);

  const char *expected[] = {
    "h_1", "q_1", "i_1", "a | b", "in_txt", "\r\n\\sum_{i=1}^n x_i\r\n"
  };
  const BtMathMode exp_modes[] = {
    BT_MATH_INLINE, BT_MATH_DISPLAY, BT_MATH_INLINE,
    BT_MATH_INLINE, BT_MATH_INLINE, BT_MATH_DISPLAY
  };
  for (guint i = 0; i < spans->len; i++) {
    BtMathSpan *sp = &g_array_index(spans, BtMathSpan, i);
    assert_int_equal(sp->mode, exp_modes[i]);
    g_autofree gchar *got = span_src(doc, sp);
    assert_string_equal(got, expected[i]);
  }
  g_array_unref(spans);

  /* Display caps: > 200 lines must not produce a span. */
  GString *huge = g_string_new("$$\n");
  for (int i = 0; i < 205; i++)
    g_string_append(huge, "a +\n");
  g_string_append(huge, "b\n$$\n");
  GArray *hs = bt_math_scan(huge->str, huge->len, NULL);
  assert_int_equal(hs->len, 0);
  g_array_unref(hs);
  g_string_free(huge, TRUE);
}

static void test_bootstrap_screenshot_regression(void **state)
{
  (void)state;
  const char *excerpt =
      "Resample with replacement from original data $B$ times:\n"
      "1. For $b = 1, \\ldots, B$: draw sample of size $n$ with replacement\n"
      "2. Compute statistic $\\hat{\\theta}^{*(b)}$ for each sample\n"
      "3. Use empirical distribution of $\\hat{\\theta}^*$ for inference\n"
      "$$(\\hat{\\theta}^*_{(\\alpha/2)}, \\hat{\\theta}^*_{(1-\\alpha/2)})$$\n"
      "1. Start with initial $\\theta^{(0)}$\n"
      "2. For $t = 1, \\ldots, T$:\n"
      "   - Propose $\\theta^* \\sim q(\\theta^* | \\theta^{(t-1)})$\n"
      "   - Compute acceptance ratio: $r = \\frac{f(\\theta^* | x) q(\\theta^{(t-1)} | \\theta^*)}{f(\\theta^{(t-1)} | x) q(\\theta^* | \\theta^{(t-1)})}$\n"
      "   - Accept $\\theta^{(t)} = \\theta^*$ with probability $\\min(1, r)$; else $\\theta^{(t)} = \\theta^{(t-1)}$\n";

  GArray *spans = bt_math_scan(excerpt, strlen(excerpt), NULL);
  assert_int_equal(spans->len, 13);

  const char *expected[13] = {
    "B",
    "b = 1, \\ldots, B",
    "n",
    "\\hat{\\theta}^{*(b)}",
    "\\hat{\\theta}^*",
    "(\\hat{\\theta}^*_{(\\alpha/2)}, \\hat{\\theta}^*_{(1-\\alpha/2)})",
    "\\theta^{(0)}",
    "t = 1, \\ldots, T",
    "\\theta^* \\sim q(\\theta^* | \\theta^{(t-1)})",
    "r = \\frac{f(\\theta^* | x) q(\\theta^{(t-1)} | \\theta^*)}{f(\\theta^{(t-1)} | x) q(\\theta^* | \\theta^{(t-1)})}",
    "\\theta^{(t)} = \\theta^*",
    "\\min(1, r)",
    "\\theta^{(t)} = \\theta^{(t-1)}"
  };
  for (guint i = 0; i < 13; i++) {
    BtMathSpan *sp = &g_array_index(spans, BtMathSpan, i);
    g_autofree gchar *got = span_src(excerpt, sp);
    assert_string_equal(got, expected[i]);
    assert_int_equal(sp->mode, i == 5 ? BT_MATH_DISPLAY : BT_MATH_INLINE);
  }
  g_array_unref(spans);
}

static void test_5k_perf_and_incremental_property(void **state)
{
  (void)state;
  /* 5,000 math spans under 20 ms (§B.21 Group T). */
  GString *big = g_string_sized_new(160000);
  for (int i = 0; i < 5000; i++) {
    if (i % 10 == 9)
      g_string_append(big, "$$\n\\sum_{k=0}^n x_k\n$$\n\n");
    else
      g_string_append(big, "Let $\\alpha_i + \\beta_i$ hold. ");
  }

  gint64 t0 = g_get_monotonic_time();
  GArray *full = bt_math_scan(big->str, big->len, NULL);
  double elapsed_ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
  printf("BT_TEST_MATH_TOK 5k_spans count=%u elapsed_ms=%.3f hb=%s\n",
         full->len, elapsed_ms, bt_math_harfbuzz_version());
  assert_int_equal(full->len, 5000);
  assert_true(elapsed_ms < 20.0);
  g_array_unref(full);
  g_string_free(big, TRUE);

  /* Seeded property test: bt_math_scan_range merged equals bt_math_scan(). */
  GString *doc = g_string_new(
      "---\ntitle: \"Prop\"\n---\n\n"
      "Para 1 $a+b$ and $c*d$.\n\n"
      "Para 2 $$\\frac{1}{2}$$ text.\n\n"
      "```c\nint x = $not_math$;\n```\n\n"
      "Para 3 $e^x$ end.\n");
  GRand *rng = g_rand_new_with_seed(0xB74A7401u);
  const char *snippets[] = { " $z_1$ ", " **bold** ", "\n\n", "$u*v$", " text ", "$$a+b$$" };

  for (int iter = 0; iter < 150; iter++) {
    gsize ins_pos = (doc->len > 0) ? (gsize)g_rand_int_range(rng, 0, (gint32)doc->len) : 0;
    const char *snip = snippets[g_rand_int_range(rng, 0, G_N_ELEMENTS(snippets))];
    g_string_insert(doc, ins_pos, snip);

    gsize bstart = 0, bend = 0;
    GArray *part = bt_math_scan_range(doc->str, doc->len, ins_pos, ins_pos + strlen(snip),
                                      NULL, &bstart, &bend);
    GArray *whole = bt_math_scan(doc->str, doc->len, NULL);

    /* Every full-scan span strictly inside [bstart, bend) must match part 1-for-1. */
    guint expected_n = 0;
    for (guint i = 0; i < whole->len; i++) {
      BtMathSpan *ws = &g_array_index(whole, BtMathSpan, i);
      if (ws->start >= bstart && ws->end <= bend) {
        assert_true(expected_n < part->len);
        BtMathSpan *ps = &g_array_index(part, BtMathSpan, expected_n);
        assert_int_equal(ps->start, ws->start);
        assert_int_equal(ps->end, ws->end);
        assert_int_equal(ps->mode, ws->mode);
        assert_int_equal(ps->hash, ws->hash);
        expected_n++;
      }
    }
    assert_int_equal(expected_n, part->len);
    g_array_unref(part);
    g_array_unref(whole);
  }

  g_rand_free(rng);
  g_string_free(doc, TRUE);
}

int main(void)
{
  setlocale(LC_NUMERIC, "C");
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_pandoc_dollar_rules),
    cmocka_unit_test(test_display_and_contexts),
    cmocka_unit_test(test_bootstrap_screenshot_regression),
    cmocka_unit_test(test_5k_perf_and_incremental_property),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
