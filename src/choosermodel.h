/* choosermodel.h — UI-free model for ambiguous wikilink candidate selection (Phase L3-2a). */
#ifndef BT_CHOOSERMODEL_H
#define BT_CHOOSERMODEL_H

#include <glib.h>

#include "vault.h"

G_BEGIN_DECLS

#define BT_CHOOSER_MAX_VISIBLE 10u

typedef struct {
  gchar *relpath;
  gchar *filename;
  gchar *folder;
} BtChooserItem;

typedef enum {
  BT_CHOOSER_OPEN_NONE_MATCH = 0,
  BT_CHOOSER_OPEN_DIRECT = 1,
  BT_CHOOSER_OPEN_SHOW = 2,
} BtChooserOpenAction;

typedef struct BtChooserModel BtChooserModel;

/* Builds an immutable candidate list from resolved NULL-terminated relpaths.
 * Preselects index 0 when count > 0. */
BtChooserModel *bt_chooser_model_new(const gchar *raw_target, const gchar *const *candidates);
void bt_chooser_model_free(BtChooserModel *m);

guint bt_chooser_model_count(const BtChooserModel *m);
guint bt_chooser_model_selected(const BtChooserModel *m);
gboolean bt_chooser_model_select(BtChooserModel *m, guint index);

guint bt_chooser_model_move_down(BtChooserModel *m);
guint bt_chooser_model_move_up(BtChooserModel *m);
guint bt_chooser_model_home(BtChooserModel *m);
guint bt_chooser_model_end(BtChooserModel *m);

guint bt_chooser_model_visible_count(const BtChooserModel *m);
guint bt_chooser_model_overflow_count(const BtChooserModel *m);

gchar *bt_chooser_model_title(const BtChooserModel *m);
gchar *bt_chooser_model_footer_more(const BtChooserModel *m);

const BtChooserItem *bt_chooser_model_item(const BtChooserModel *m, guint index);
const gchar *bt_chooser_model_selected_relpath(const BtChooserModel *m);

gchar *bt_chooser_model_copy_link(const BtChooserModel *m, guint index);
gchar *bt_chooser_model_copy_status(const BtChooserModel *m, guint index);

BtChooserOpenAction bt_chooser_decide_open(const gchar *const *candidates);
gboolean bt_chooser_candidate_exists(const BtVault *vault, const gchar *relpath,
                                     gchar **out_status_msg);

G_END_DECLS

#endif
