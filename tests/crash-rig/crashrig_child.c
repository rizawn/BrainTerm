/* crashrig_child.c — performs one atomic write of the "NEW" payload and kills
 * itself hard at the requested stage (§11.3). Usage:
 *   crashrig_child <vault> <relpath> <stage>   (stage == COUNT: no crash) */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "testutil.h"
#include "vault.h"

#define CRASHRIG_LINES 300000u
#define CRASHRIG_KILLED 137

static void die_hard(void)
{
#ifdef G_OS_WIN32
  TerminateProcess(GetCurrentProcess(), CRASHRIG_KILLED);
#else
  kill(getpid(), SIGKILL);
#endif
  abort(); /* unreachable */
}

static void at_stage(BtWriteStage stage, void *ud)
{
  if ((int)stage == *(int *)ud)
    die_hard();
}

int main(int argc, char **argv)
{
  if (argc != 4) {
    fprintf(stderr, "usage: %s <vault> <relpath> <stage>\n", argv[0]);
    return 2;
  }
  int stage = atoi(argv[3]);
  GError *err = NULL;
  BtVault *v = vault_open(argv[1], NULL, &err);
  if (v == NULL) {
    fprintf(stderr, "open: %s\n", err->message);
    return 3;
  }
  gsize len = 0;
  gchar *data = bt_test_payload("NEW", CRASHRIG_LINES, &len);
  BtWriteHooks hooks = {at_stage, &stage};
  if (!vault_atomic_write(v, argv[2], data, len, &hooks, &err)) {
    fprintf(stderr, "write: %s\n", err->message);
    return 4;
  }
  g_free(data);
  vault_free(v);
  return 0;
}
