/* bench_createnote.c — §8 budget: planner latency on 3,000-note snapshot (< 5 ms budget). */
#include <glib.h>
#include <locale.h>
#include <stdio.h>

#include "createnote.h"
#include "linkresolve.h"

#define BT_PLAN_BUDGET_MS 5.0

int main(void)
{
  setlocale(LC_NUMERIC, "C");
  BtIndexSnapshot *snap = bt_index_snapshot_new();
  for (guint i = 0; i < 3000u; i++) {
    g_autofree gchar *rel = g_strdup_printf("area_%02u/topic_%03u/note_%04u.md",
                                            (i / 300u) % 10u, (i / 30u) % 100u, i);
    g_autofree gchar *title = g_strdup_printf("Note %04u", i);
    bt_index_snapshot_add_entry(snap, rel, FALSE, title, NULL);
  }
  bt_index_snapshot_finalize(snap);

  /* Warm once */
  BtCreatePlan *pw = bt_createnote_plan(NULL, snap, "area_00/topic_000/note_0000.md", "fresh-note");
  bt_createnote_plan_free(pw);

  double max_ms = 0.0;
  double total_ms = 0.0;
  const guint iters = 200u;
  for (guint i = 0; i < iters; i++) {
    g_autofree gchar *target =
        (i % 2u == 0u) ? g_strdup_printf("brand_new_%u", i) : g_strdup_printf("note_%04u", i % 3000u);
    gint64 t0 = g_get_monotonic_time();
    BtCreatePlan *p = bt_createnote_plan(NULL, snap, "area_00/topic_000/note_0000.md", target);
    double ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
    total_ms += ms;
    if (ms > max_ms)
      max_ms = ms;
    bt_createnote_plan_free(p);
  }

  double avg_ms = total_ms / (double)iters;
  printf("BT_BENCH_CREATENOTE 3k_notes iters=%u avg_ms=%.4f max_ms=%.4f budget_ms=%.1f\n",
         iters, avg_ms, max_ms, BT_PLAN_BUDGET_MS);
  bt_index_snapshot_free(snap);
  return (avg_ms < BT_PLAN_BUDGET_MS && max_ms < BT_PLAN_BUDGET_MS) ? 0 : 1;
}
