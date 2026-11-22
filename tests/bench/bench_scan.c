/* bench_scan.c — §10 budget: vault cold scan. Usage: bench_scan <notes>
 * Creates <notes> notes (plus noise and ignored dirs), then times
 * vault_open(). Fails (exit 1) if over BT_SCAN_BUDGET_MS. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#include "testutil.h"
#include "vault.h"

#define BT_SCAN_BUDGET_MS 2000.0

int main(int argc, char **argv)
{
  guint n = argc > 1 ? (guint)strtoul(argv[1], NULL, 10) : 1000u;
  if (n == 0)
    n = 1000u;
  gchar *root = g_dir_make_tmp("bt-bench-XXXXXX", NULL);
  if (root == NULL)
    return 2;

  /* ~50 notes per folder, 3 levels, like a real vault; plus noise. */
  for (guint i = 0; i < n; i++) {
    gchar *rel = g_strdup_printf("area-%02u/proj-%03u/note-%06u.md", (i / 2500) % 100, i / 50, i);
    bt_test_put(root, rel, "---\ntitle: x\n---\n# heading\nbody [[link]]\n");
    g_free(rel);
  }
  for (guint i = 0; i < n / 10; i++) {
    gchar *rel = g_strdup_printf("attachments/img-%06u.png", i);
    bt_test_put(root, rel, "png");
    g_free(rel);
    rel = g_strdup_printf(".git/objects/%06u", i);
    bt_test_put(root, rel, "obj");
    g_free(rel);
  }

  BtScanStats st;
  GError *err = NULL;
  BtVault *v = vault_open(root, &st, &err);
  if (v == NULL) {
    fprintf(stderr, "open failed: %s\n", err->message);
    return 2;
  }
  double ms = (double)st.elapsed_us / 1000.0;
  printf("BT_BENCH vault_scan notes=%u found=%u elapsed_ms=%.1f budget_ms=%.0f\n", n, st.notes, ms,
         BT_SCAN_BUDGET_MS);
  int rc = (st.notes == n && ms < BT_SCAN_BUDGET_MS) ? 0 : 1;
  vault_free(v);
  bt_test_rm_rf(root);
  g_free(root);
  return rc;
}
