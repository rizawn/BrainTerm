/* vtermbridge.h — libvterm state machine + scrollback ring + grid mirror (§5.2, §6.D3, §8.2). */
#ifndef BT_VTERMBRIDGE_H
#define BT_VTERMBRIDGE_H

#include <glib.h>
#include <vterm.h>

G_BEGIN_DECLS

#define BT_SCROLLBACK_DEFAULT 10000u
#define BT_SCROLLBACK_MIN     1000u
#define BT_SCROLLBACK_MAX     100000u

/* Compact cell representation (16 bytes) for both screen mirror and scrollback. */
typedef enum {
  BT_CELL_BOLD      = 1u << 0,
  BT_CELL_ITALIC    = 1u << 1,
  BT_CELL_UNDERLINE = 1u << 2,
  BT_CELL_STRIKE    = 1u << 3,
  BT_CELL_REVERSE   = 1u << 4,
  BT_CELL_CONCEAL   = 1u << 5,
} BtCellFlags;

typedef struct {
  guint32 ch;     /* primary Unicode codepoint (0 = blank/continuation) */
  guint8 width;   /* 1 or 2; 0 = right half of a wide char */
  guint8 flags;   /* BtCellFlags */
  guint16 pad;
  guint32 fg_rgb; /* 0x00RRGGBB */
  guint32 bg_rgb; /* 0x00RRGGBB */
} BtCell;

typedef struct BtVTerm BtVTerm;

typedef void (*BtVTermOutputFn)(const guint8 *bytes, gsize len, gpointer user_data);
typedef void (*BtVTermTitleFn)(const gchar *title, gpointer user_data);

BtVTerm *bt_vterm_new(gint rows, gint cols, guint scrollback_cap, BtVTermOutputFn on_output,
                      gpointer user_data);
void bt_vterm_free(BtVTerm *vt);

void bt_vterm_set_title_cb(BtVTerm *vt, BtVTermTitleFn on_title, gpointer user_data);

/* Hot-path input: feeds raw PTY bytes into libvterm and flushes damage. */
void bt_vterm_feed(BtVTerm *vt, const guint8 *bytes, gsize len);

/* Resizes the terminal grid and triggers a full-damage frame. */
void bt_vterm_resize(BtVTerm *vt, gint rows, gint cols);

/* Keyboard & mouse helpers forwarding through libvterm -> on_output. */
void bt_vterm_key(BtVTerm *vt, VTermKey key, VTermModifier mod);
void bt_vterm_unichar(BtVTerm *vt, guint32 codepoint, VTermModifier mod);
void bt_vterm_paste(BtVTerm *vt, const gchar *utf8, gsize len);
void bt_vterm_mouse_button(BtVTerm *vt, gint button, gboolean pressed, VTermModifier mod);
void bt_vterm_mouse_move(BtVTerm *vt, gint row, gint col, VTermModifier mod);

/* Grid geometry & state queries. */
gint bt_vterm_rows(const BtVTerm *vt);
gint bt_vterm_cols(const BtVTerm *vt);
guint bt_vterm_scrollback_len(const BtVTerm *vt);
gboolean bt_vterm_is_altscreen(const BtVTerm *vt);
gint bt_vterm_mouse_mode(const BtVTerm *vt);
gboolean bt_vterm_osc52_enabled(const BtVTerm *vt);
const gchar *bt_vterm_title(const BtVTerm *vt);

void bt_vterm_cursor(const BtVTerm *vt, gint *row, gint *col, gboolean *visible, gint *shape);
guint32 bt_vterm_default_fg(const BtVTerm *vt);
guint32 bt_vterm_default_bg(const BtVTerm *vt);

/* Virtual row indexing:
 *   0 .. scrollback_len-1               = oldest..newest scrollback lines
 *   scrollback_len .. scrollback+rows-1 = live screen rows 0..rows-1 */
void bt_vterm_get_vrow_cell(const BtVTerm *vt, guint vrow, gint col, BtCell *out);

/* Returns dirty screen row range [start, end) and clears the dirty flag. */
gboolean bt_vterm_take_damage(BtVTerm *vt, gint *start_row, gint *end_row);

/* Extracts UTF-8 text from virtual coordinates (inclusive start..end). */
gchar *bt_vterm_extract_text(const BtVTerm *vt, guint start_vrow, gint start_col, guint end_vrow,
                             gint end_col);

/* Case-insensitive substring search across scrollback + screen.
 * Returns a GArray of guint virtual row indices (caller frees). */
GArray *bt_vterm_search(const BtVTerm *vt, const gchar *query);

G_END_DECLS

#endif
