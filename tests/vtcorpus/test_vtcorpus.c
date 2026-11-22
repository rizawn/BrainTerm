/* test_vtcorpus.c — §11.2 Terminal conformance corpus: replays scripted VT
 * streams through BtVTerm (libvterm + scrollback + grid mirror) and asserts
 * exact cell grids, RGB colors, attributes, altscreen, scrollback, resize,
 * keyboard DECCKM output, and OSC 52 disabled (§8.2). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "vtermbridge.h"

static void feed_str(BtVTerm *vt, const gchar *s)
{
  bt_vterm_feed(vt, (const guint8 *)s, strlen(s));
}

static gchar *row_text(const BtVTerm *vt, guint vrow)
{
  return bt_vterm_extract_text(vt, vrow, 0, vrow, bt_vterm_cols(vt) - 1);
}

static void test_sgr_colors_and_attrs(void **state)
{
  (void)state;
  BtVTerm *vt = bt_vterm_new(6, 40, 100, NULL, NULL);
  /* Bold + italic + underline + truecolor RGB fg/bg */
  feed_str(vt, "\x1b[1;3;4;38;2;255;128;64;48;2;10;20;30mHi\x1b[0m!");

  BtCell c0, c1, c2;
  bt_vterm_get_vrow_cell(vt, 0, 0, &c0);
  bt_vterm_get_vrow_cell(vt, 0, 1, &c1);
  bt_vterm_get_vrow_cell(vt, 0, 2, &c2);

  assert_int_equal(c0.ch, 'H');
  assert_int_equal(c0.fg_rgb, 0x00FF8040u);
  assert_int_equal(c0.bg_rgb, 0x000A141Eu);
  assert_true((c0.flags & BT_CELL_BOLD) != 0);
  assert_true((c0.flags & BT_CELL_ITALIC) != 0);
  assert_true((c0.flags & BT_CELL_UNDERLINE) != 0);

  assert_int_equal(c1.ch, 'i');
  assert_int_equal(c1.fg_rgb, 0x00FF8040u);

  assert_int_equal(c2.ch, '!');
  assert_int_equal(c2.flags, 0);
  assert_int_equal(c2.fg_rgb, bt_vterm_default_fg(vt));
  assert_int_equal(c2.bg_rgb, bt_vterm_default_bg(vt));

  bt_vterm_free(vt);
}

static void test_altscreen_vim_tui(void **state)
{
  (void)state;
  BtVTerm *vt = bt_vterm_new(5, 20, 100, NULL, NULL);
  feed_str(vt, "shell prompt$ ");
  assert_false(bt_vterm_is_altscreen(vt));

  /* Enter altscreen (DECSET 1049), draw status bar at row 4, leave altscreen. */
  feed_str(vt, "\x1b[?1049h\x1b[1;1H~ vim buffer\x1b[5;1H\x1b[7m-- NORMAL --\x1b[0m");
  assert_true(bt_vterm_is_altscreen(vt));

  gchar *r0 = row_text(vt, 0);
  gchar *r4 = row_text(vt, 4);
  assert_string_equal(r0, "~ vim buffer");
  assert_string_equal(r4, "-- NORMAL --");
  g_free(r0);
  g_free(r4);

  BtCell rev;
  bt_vterm_get_vrow_cell(vt, 4, 0, &rev);
  assert_true((rev.flags & BT_CELL_REVERSE) != 0);

  /* Leave altscreen: original main buffer restored. */
  feed_str(vt, "\x1b[?1049l");
  assert_false(bt_vterm_is_altscreen(vt));
  gchar *restored = row_text(vt, 0);
  assert_string_equal(restored, "shell prompt$");
  g_free(restored);

  bt_vterm_free(vt);
}

static void test_scrollback_ring_and_search(void **state)
{
  (void)state;
  /* Small ring (cap = 20) to exercise wrap-around eviction. */
  BtVTerm *vt = bt_vterm_new(4, 30, 20, NULL, NULL);
  for (int i = 0; i < 35; i++) {
    gchar *line = g_strdup_printf("line-%02d %s\r\n", i, (i == 18 || i == 33) ? "NEEDLE" : "ok");
    feed_str(vt, line);
    g_free(line);
  }
  assert_int_equal(bt_vterm_scrollback_len(vt), 20);

  /* Search case-insensitively across scrollback + screen. */
  GArray *hits = bt_vterm_search(vt, "needle");
  assert_int_equal(hits->len, 2);
  gchar *h0 = row_text(vt, g_array_index(hits, guint, 0));
  gchar *h1 = row_text(vt, g_array_index(hits, guint, 1));
  assert_non_null(strstr(h0, "line-18 NEEDLE"));
  assert_non_null(strstr(h1, "line-33 NEEDLE"));
  g_free(h0);
  g_free(h1);
  g_array_unref(hits);

  bt_vterm_free(vt);
}

static void test_utf8_wide_chars_and_resize(void **state)
{
  (void)state;
  BtVTerm *vt = bt_vterm_new(4, 20, 50, NULL, NULL);
  /* CJK wide character '漢' (U+6F22) takes 2 cells. */
  feed_str(vt, "A\xE6\xBC\xA2" "B");

  BtCell ca, cw, ccont, cb;
  bt_vterm_get_vrow_cell(vt, 0, 0, &ca);
  bt_vterm_get_vrow_cell(vt, 0, 1, &cw);
  bt_vterm_get_vrow_cell(vt, 0, 2, &ccont);
  bt_vterm_get_vrow_cell(vt, 0, 3, &cb);
  assert_int_equal(ca.ch, 'A');
  assert_int_equal(cw.ch, 0x6F22u);
  assert_int_equal(cw.width, 2);
  assert_int_equal(ccont.width, 0);
  assert_int_equal(cb.ch, 'B');

  bt_vterm_resize(vt, 10, 40);
  assert_int_equal(bt_vterm_rows(vt), 10);
  assert_int_equal(bt_vterm_cols(vt), 40);
  gint r0 = 0, r1 = 0;
  assert_true(bt_vterm_take_damage(vt, &r0, &r1));
  assert_int_equal(r0, 0);
  assert_int_equal(r1, 10);

  bt_vterm_free(vt);
}

static void capture_out(const guint8 *bytes, gsize len, gpointer ud)
{
  GString *s = ud;
  g_string_append_len(s, (const gchar *)bytes, (gssize)len);
}

static void test_decckm_and_osc52_disabled(void **state)
{
  (void)state;
  GString *out = g_string_new(NULL);
  BtVTerm *vt = bt_vterm_new(6, 30, 50, capture_out, out);

  /* OSC 52 must be disabled by default (§8.2); feeding it must produce no output/crash. */
  assert_false(bt_vterm_osc52_enabled(vt));
  feed_str(vt, "\x1b]52;c;c2VjcmV0\x07");
  assert_int_equal(out->len, 0);

  /* Normal cursor keys -> ESC [ A; DECCKM (?1h) -> ESC O A. */
  bt_vterm_key(vt, VTERM_KEY_UP, VTERM_MOD_NONE);
  assert_string_equal(out->str, "\x1b[A");
  g_string_truncate(out, 0);

  feed_str(vt, "\x1b[?1h");
  bt_vterm_key(vt, VTERM_KEY_UP, VTERM_MOD_NONE);
  assert_string_equal(out->str, "\x1bOA");

  g_string_free(out, TRUE);
  bt_vterm_free(vt);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sgr_colors_and_attrs),
    cmocka_unit_test(test_altscreen_vim_tui),
    cmocka_unit_test(test_scrollback_ring_and_search),
    cmocka_unit_test(test_utf8_wide_chars_and_resize),
    cmocka_unit_test(test_decckm_and_osc52_disabled),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
