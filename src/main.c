/* main.c — BrainTerm entry point (§5.2). */
#include "app.h"

int main(int argc, char **argv)
{
  BtApp *app = bt_app_new();
  int status = bt_app_run(app, argc, argv);
  bt_app_free(app);
  return status;
}
