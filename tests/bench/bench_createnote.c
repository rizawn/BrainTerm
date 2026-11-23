/* bench_createnote.c — §8 budget: planner latency on 3,000-note snapshot (< 5 ms budget). */
#include <glib.h>
#include <locale.h>
#include <stdio.h>

#include "createnote.h"
#include "index.h"
#include "linkresolve.h"
#include "vault.h"

#define BT_PLAN_BUDGET_MS 5.0

static int run_reresolve_bench(const char *vault_copy_dir)
{
  BtVault *v = vault_open(vault_copy_dir, NULL, NULL);
  if (v == NULL)
    return 2;
  BtIndex *idx = bt_index_open(vault_copy_dir, NULL, NULL);
  if (idx == NULL) {
    vault_free(v);
    return 2;
  }
  bt_index_rebuild(idx, v, NULL);
  BtIndexSnapshot *snap = bt_index_snapshot_from_vault(v);
  BtCreatePlan *p = bt_createnote_plan(v, snap, "Welcome.md", "note-1");
  if (p == NULL || p->result != BT_CREATE_PLAN_OK) {
    bt_createnote_plan_free(p);
    bt_index_snapshot_free(snap);
    bt_index_close(idx);
    vault_free(v);
    return 3;
  }
  g_autofree gchar *rel = NULL;
  if (bt_createnote_commit(v, p, &rel, NULL, NULL, NULL) != BT_CREATE_COMMIT_OK) {
    bt_createnote_plan_free(p);
    bt_index_snapshot_free(snap);
    bt_index_close(idx);
    vault_free(v);
    return 4;
  }
  gint64 t0 = g_get_monotonic_time();
  gboolean ok = bt_index_upsert_after_create(idx, v, rel, NULL);
  double ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
  printf("BT_BENCH_RERESOLVE notes=%u reresolve_ms=%.3f budget_ms=50.0\n",
         vault_notes(v)->len, ms);
  bt_createnote_plan_free(p);
  bt_index_snapshot_free(snap);
  bt_index_close(idx);
  vault_free(v);
  return (ok && ms < 50.0) ? 0 : 1;
}

int main(int argc, char **argv)
{
  setlocale(LC_NUMERIC, "C");
  if (argc >= 3 && g_strcmp0(argv[1], "--reresolve") == 0)
    return run_reresolve_bench(argv[2]);
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
