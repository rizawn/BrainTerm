/* bench_vterm.c — §10 budget: terminal full-screen redraw & keystroke->grid
 * processing latency (budget < 16 ms / 60 fps sustained). */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "vtermbridge.h"

#define BT_VTERM_FRAMES 1000u
#define BT_FRAME_BUDGET_MS 16.0

int main(void)
{
  BtVTerm *vt = bt_vterm_new(24, 80, BT_SCROLLBACK_DEFAULT, NULL, NULL);

  /* Build a realistic full-screen vim-like colored frame (~3.5 KB). */
  GString *frame = g_string_sized_new(4096);
  g_string_append(frame, "\x1b[H");
  for (int r = 0; r < 23; r++) {
    g_string_append_printf(
        frame, "\x1b[%d;1H\x1b[38;2;120;180;255m%4d \x1b[0mstatic int fn_%02d(int x) { return x + %d; }\x1b[K",
        r + 1, r + 100, r, r);
  }
  g_string_append(frame, "\x1b[24;1H\x1b[7m src/terminal.c [+]   80x24   100% \x1b[0m\x1b[K");

  gint64 t0 = g_get_monotonic_time();
  for (guint i = 0; i < BT_VTERM_FRAMES; i++) {
    bt_vterm_feed(vt, (const guint8 *)frame->str, frame->len);
    gint r0 = 0, r1 = 0;
    bt_vterm_take_damage(vt, &r0, &r1);
  }
  gint64 elapsed_us = g_get_monotonic_time() - t0;
  double ms_per_frame = ((double)elapsed_us / 1000.0) / (double)BT_VTERM_FRAMES;

  printf("BT_BENCH vterm_redraw frames=%u avg_ms=%.3f budget_ms=%.1f\n", BT_VTERM_FRAMES,
         ms_per_frame, BT_FRAME_BUDGET_MS);

  g_string_free(frame, TRUE);
  bt_vterm_free(vt);
  return ms_per_frame < BT_FRAME_BUDGET_MS ? 0 : 1;
}
