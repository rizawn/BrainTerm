/* editorpane.h — GtkSourceView 5 Markdown editor pane (§6.A2, ADR-004). */
#ifndef BT_EDITORPANE_H
#define BT_EDITORPANE_H

#include <gtk/gtk.h>

#include "linkresolve.h"

G_BEGIN_DECLS

typedef struct BtEditorPane BtEditorPane;

typedef void (*BtEditorSaveFn)(const gchar *relpath, const gchar *text, gsize length,
                               gpointer user_data);
typedef void (*BtEditorLinkFn)(const gchar *target, gpointer user_data);
typedef void (*BtEditorDirtyFn)(gboolean dirty, gpointer user_data);
typedef void (*BtEditorDiffFn)(const gchar *relpath, gpointer user_data);
typedef void (*BtEditorReloadFn)(const gchar *relpath, gpointer user_data);

BtEditorPane *bt_editorpane_new(BtEditorSaveFn on_save, BtEditorLinkFn on_link,
                                BtEditorDirtyFn on_dirty, gpointer user_data);
void bt_editorpane_set_conflict_cbs(BtEditorPane *pane, BtEditorDiffFn on_diff,
                                    BtEditorReloadFn on_reload);
void bt_editorpane_show_conflict(BtEditorPane *pane, gboolean deleted);
void bt_editorpane_hide_conflict(BtEditorPane *pane);
void bt_editorpane_set_dark(BtEditorPane *pane, gboolean dark);
void bt_editorpane_set_line_numbers(BtEditorPane *pane, gboolean show);
void bt_editorpane_set_links_open_on_click(BtEditorPane *pane, gboolean open_on_click);
gboolean bt_editorpane_get_links_open_on_click(const BtEditorPane *pane);
gchar *bt_editorpane_buffer_hash(const BtEditorPane *pane);
void bt_editorpane_free(BtEditorPane *pane);

GtkWidget *bt_editorpane_widget(BtEditorPane *pane);
GtkWidget *bt_editorpane_view_widget(BtEditorPane *pane);

/* Loads a note into the editor buffer; clears dirty flag and pending autosave. */
void bt_editorpane_load(BtEditorPane *pane, const gchar *relpath, const gchar *text, gsize length);
void bt_editorpane_clear(BtEditorPane *pane);

const gchar *bt_editorpane_relpath(const BtEditorPane *pane);
gboolean bt_editorpane_is_dirty(const BtEditorPane *pane);
const gchar *bt_editorpane_scheme_id(const BtEditorPane *pane);

/* Triggers an immediate save if a note is open (cancels autosave timer). */
void bt_editorpane_flush_save(BtEditorPane *pane);

/* Called by BtApp after vault_atomic_write succeeds. */
void bt_editorpane_mark_saved(BtEditorPane *pane);

/* Inspection & incremental link model helpers (L2 / Group H). */
const GArray *bt_editorpane_link_spans(const BtEditorPane *pane);
gboolean bt_editorpane_link_at_offset(const BtEditorPane *pane, gsize byte_offset, BtLinkSpan *out_span);
gsize bt_editorpane_caret_byte_offset(const BtEditorPane *pane);
void bt_editorpane_set_caret_byte_offset(BtEditorPane *pane, gsize byte_offset);
gboolean bt_editorpane_simulate_click(BtEditorPane *pane, gsize target_byte_offset, guint button,
                                      gint n_press, gdouble drag_dx, gdouble drag_dy,
                                      GdkModifierType mods);
gboolean bt_editorpane_range_has_emphasis_tag(const BtEditorPane *pane, gsize byte_start, gsize byte_end);
gboolean bt_editorpane_offset_has_tag_name(const BtEditorPane *pane, gsize byte_offset, const gchar *tag_name);
const gchar *bt_editorpane_language_id(const BtEditorPane *pane);

G_END_DECLS

#endif
