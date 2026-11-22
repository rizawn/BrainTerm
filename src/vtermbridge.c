/* vtermbridge.c — libvterm + scrollback ring buffer + grid mirror (§5.4, §6.D3, §8.2). */
#include "vtermbridge.h"
#include "theme.h"

#include <string.h>

#define BT_DEFAULT_FG BT_COL_TEXT_PRIMARY
#define BT_DEFAULT_BG BT_COL_SURFACE_1

typedef struct {
  BtCell *cells;
  gint len; /* trimmed length (trailing blanks omitted) */
} ScrollLine;

struct BtVTerm {
  VTerm *vt;
  VTermScreen *screen;
  VTermState *state;

  gint rows;
  gint cols;
  BtCell *grid; /* rows * cols live mirror */

  /* Scrollback ring buffer (§6.D3). */
  ScrollLine *sb;
  guint sb_cap;
  guint sb_head; /* oldest entry index */
  guint sb_len;

  /* Damage coalescing. */
  gboolean dirty;
  gint dirty_start;
  gint dirty_end;

  /* Terminal properties. */
  gint cursor_row;
  gint cursor_col;
  gboolean cursor_visible;
  gint cursor_shape;
  gboolean altscreen;
  gint mouse_mode;
  gboolean osc52_enabled; /* FALSE by default (§8.2) */
  gchar *title;

  BtVTermOutputFn on_output;
  gpointer output_ud;
  BtVTermTitleFn on_title;
  gpointer title_ud;
};

static inline guint32 color_to_rgb(const BtVTerm *bt, VTermColor c, guint32 def_rgb)
{
  if (VTERM_COLOR_IS_DEFAULT_FG(&c))
    return BT_DEFAULT_FG;
  if (VTERM_COLOR_IS_DEFAULT_BG(&c))
    return BT_DEFAULT_BG;
  if (VTERM_COLOR_IS_INDEXED(&c))
    vterm_screen_convert_color_to_rgb(bt->screen, &c);
  if (VTERM_COLOR_IS_RGB(&c))
    return ((guint32)c.rgb.red << 16) | ((guint32)c.rgb.green << 8) | (guint32)c.rgb.blue;
  return def_rgb;
}

static void convert_cell(const BtVTerm *bt, const VTermScreenCell *src, BtCell *dst)
{
  if (src->chars[0] == (uint32_t)-1) {
    dst->ch = 0;
    dst->width = 0;
  } else {
    dst->ch = src->chars[0];
    dst->width = (guint8)MAX(src->width, 0);
  }
  guint8 fl = 0;
  if (src->attrs.bold)
    fl |= BT_CELL_BOLD;
  if (src->attrs.italic)
    fl |= BT_CELL_ITALIC;
  if (src->attrs.underline != VTERM_UNDERLINE_OFF)
    fl |= BT_CELL_UNDERLINE;
  if (src->attrs.strike)
    fl |= BT_CELL_STRIKE;
  if (src->attrs.reverse)
    fl |= BT_CELL_REVERSE;
  if (src->attrs.conceal)
    fl |= BT_CELL_CONCEAL;
  dst->flags = fl;
  dst->pad = 0;
  dst->fg_rgb = color_to_rgb(bt, src->fg, BT_DEFAULT_FG);
  dst->bg_rgb = color_to_rgb(bt, src->bg, BT_DEFAULT_BG);
}

static BtCell blank_cell(void)
{
  BtCell c;
  c.ch = 0;
  c.width = 1;
  c.flags = 0;
  c.pad = 0;
  c.fg_rgb = BT_DEFAULT_FG;
  c.bg_rgb = BT_DEFAULT_BG;
  return c;
}

static void mark_dirty(BtVTerm *bt, gint r0, gint r1)
{
  r0 = CLAMP(r0, 0, bt->rows);
  r1 = CLAMP(r1, 0, bt->rows);
  if (r0 >= r1)
    return;
  if (!bt->dirty) {
    bt->dirty = TRUE;
    bt->dirty_start = r0;
    bt->dirty_end = r1;
  } else {
    bt->dirty_start = MIN(bt->dirty_start, r0);
    bt->dirty_end = MAX(bt->dirty_end, r1);
  }
}

static void sync_rect_to_grid(BtVTerm *bt, VTermRect rect)
{
  gint r0 = CLAMP(rect.start_row, 0, bt->rows);
  gint r1 = CLAMP(rect.end_row, 0, bt->rows);
  gint c0 = CLAMP(rect.start_col, 0, bt->cols);
  gint c1 = CLAMP(rect.end_col, 0, bt->cols);

  VTermScreenCell vcell;
  for (gint r = r0; r < r1; r++) {
    BtCell *row_dst = bt->grid + (gsize)r * (gsize)bt->cols;
    for (gint c = c0; c < c1; c++) {
      VTermPos pos = {r, c};
      vterm_screen_get_cell(bt->screen, pos, &vcell);
      convert_cell(bt, &vcell, &row_dst[c]);
    }
  }
  mark_dirty(bt, r0, r1);
}

/* --------------------------------------------------- libvterm callbacks -- */

static int cb_damage(VTermRect rect, void *user)
{
  sync_rect_to_grid(user, rect);
  return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  BtVTerm *bt = user;
  mark_dirty(bt, oldpos.row, oldpos.row + 1);
  bt->cursor_row = pos.row;
  bt->cursor_col = pos.col;
  bt->cursor_visible = visible != 0;
  mark_dirty(bt, pos.row, pos.row + 1);
  return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
  BtVTerm *bt = user;
  switch (prop) {
  case VTERM_PROP_CURSORVISIBLE:
    bt->cursor_visible = val->boolean != 0;
    mark_dirty(bt, bt->cursor_row, bt->cursor_row + 1);
    break;
  case VTERM_PROP_CURSORSHAPE:
    bt->cursor_shape = val->number;
    mark_dirty(bt, bt->cursor_row, bt->cursor_row + 1);
    break;
  case VTERM_PROP_ALTSCREEN:
    bt->altscreen = val->boolean != 0;
    mark_dirty(bt, 0, bt->rows);
    break;
  case VTERM_PROP_MOUSE:
    bt->mouse_mode = val->number;
    break;
  case VTERM_PROP_TITLE:
    if (val->string.initial) {
      g_free(bt->title);
      bt->title = g_strndup(val->string.str, val->string.len);
    } else if (bt->title != NULL) {
      gchar *part = g_strndup(val->string.str, val->string.len);
      gchar *joined = g_strconcat(bt->title, part, NULL);
      g_free(part);
      g_free(bt->title);
      bt->title = joined;
    }
    if (val->string.final && bt->on_title != NULL && bt->title != NULL)
      bt->on_title(bt->title, bt->title_ud);
    break;
  default:
    break;
  }
  return 1;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
  BtVTerm *bt = user;
  if (bt->sb_cap == 0)
    return 0;

  /* Trim trailing blank default-colored cells to save memory (§14.4). */
  gint len = cols;
  while (len > 0) {
    const VTermScreenCell *c = &cells[len - 1];
    if ((c->chars[0] == 0 || c->chars[0] == ' ') && VTERM_COLOR_IS_DEFAULT_BG(&c->bg) &&
        !c->attrs.reverse)
      len--;
    else
      break;
  }

  guint slot;
  if (bt->sb_len < bt->sb_cap) {
    slot = (bt->sb_head + bt->sb_len) % bt->sb_cap;
    bt->sb_len++;
  } else {
    slot = bt->sb_head;
    bt->sb_head = (bt->sb_head + 1) % bt->sb_cap;
    g_free(bt->sb[slot].cells);
  }

  bt->sb[slot].len = len;
  if (len > 0) {
    bt->sb[slot].cells = g_new(BtCell, (gsize)len);
    for (gint i = 0; i < len; i++)
      convert_cell(bt, &cells[i], &bt->sb[slot].cells[i]);
  } else {
    bt->sb[slot].cells = NULL;
  }
  return 1;
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
  BtVTerm *bt = user;
  if (bt->sb_len == 0)
    return 0;

  bt->sb_len--;
  guint slot = (bt->sb_head + bt->sb_len) % bt->sb_cap;
  ScrollLine *line = &bt->sb[slot];

  VTermColor def_fg, def_bg;
  vterm_state_get_default_colors(bt->state, &def_fg, &def_bg);

  for (gint i = 0; i < cols; i++) {
    memset(&cells[i], 0, sizeof(cells[i]));
    if (i < line->len && line->cells != NULL) {
      const BtCell *src = &line->cells[i];
      cells[i].chars[0] = src->ch;
      cells[i].width = (char)src->width;
      cells[i].attrs.bold = (src->flags & BT_CELL_BOLD) != 0;
      cells[i].attrs.italic = (src->flags & BT_CELL_ITALIC) != 0;
      cells[i].attrs.underline = (src->flags & BT_CELL_UNDERLINE) ? VTERM_UNDERLINE_SINGLE : 0;
      cells[i].attrs.strike = (src->flags & BT_CELL_STRIKE) != 0;
      cells[i].attrs.reverse = (src->flags & BT_CELL_REVERSE) != 0;
      cells[i].attrs.conceal = (src->flags & BT_CELL_CONCEAL) != 0;
      vterm_color_rgb(&cells[i].fg, (uint8_t)((src->fg_rgb >> 16) & 0xFF),
                      (uint8_t)((src->fg_rgb >> 8) & 0xFF), (uint8_t)(src->fg_rgb & 0xFF));
      vterm_color_rgb(&cells[i].bg, (uint8_t)((src->bg_rgb >> 16) & 0xFF),
                      (uint8_t)((src->bg_rgb >> 8) & 0xFF), (uint8_t)(src->bg_rgb & 0xFF));
    } else {
      cells[i].width = 1;
      cells[i].fg = def_fg;
      cells[i].bg = def_bg;
    }
  }
  g_free(line->cells);
  line->cells = NULL;
  line->len = 0;
  return 1;
}

static int cb_sb_clear(void *user)
{
  BtVTerm *bt = user;
  for (guint i = 0; i < bt->sb_len; i++) {
    guint slot = (bt->sb_head + i) % bt->sb_cap;
    g_free(bt->sb[slot].cells);
    bt->sb[slot].cells = NULL;
    bt->sb[slot].len = 0;
  }
  bt->sb_head = 0;
  bt->sb_len = 0;
  return 1;
}

static void cb_output(const char *s, size_t len, void *user)
{
  BtVTerm *bt = user;
  if (bt->on_output != NULL && len > 0)
    bt->on_output((const guint8 *)s, len, bt->output_ud);
}

static const VTermScreenCallbacks screen_cbs = {
  .damage = cb_damage,
  .moverect = NULL,
  .movecursor = cb_movecursor,
  .settermprop = cb_settermprop,
  .bell = NULL,
  .resize = NULL,
  .sb_pushline = cb_sb_pushline,
  .sb_popline = cb_sb_popline,
  .sb_clear = cb_sb_clear,
};

/* ---------------------------------------------------------- public API -- */

BtVTerm *bt_vterm_new(gint rows, gint cols, guint scrollback_cap, BtVTermOutputFn on_output,
                      gpointer user_data)
{
  rows = MAX(rows, 2);
  cols = MAX(cols, 2);
  if (scrollback_cap == 0)
    scrollback_cap = BT_SCROLLBACK_DEFAULT;

  BtVTerm *bt = g_new0(BtVTerm, 1);
  bt->rows = rows;
  bt->cols = cols;
  bt->grid = g_new(BtCell, (gsize)rows * (gsize)cols);
  for (gsize i = 0; i < (gsize)rows * (gsize)cols; i++)
    bt->grid[i] = blank_cell();

  bt->sb_cap = scrollback_cap;
  bt->sb = g_new0(ScrollLine, scrollback_cap);
  bt->cursor_visible = TRUE;
  bt->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
  bt->osc52_enabled = FALSE; /* §8.2: off by default */
  bt->on_output = on_output;
  bt->output_ud = user_data;

  bt->vt = vterm_new(rows, cols);
  vterm_set_utf8(bt->vt, 1);
  vterm_output_set_callback(bt->vt, cb_output, bt);

  bt->state = vterm_obtain_state(bt->vt);
  VTermColor def_fg, def_bg;
  vterm_color_rgb(&def_fg, (BT_DEFAULT_FG >> 16) & 0xFF, (BT_DEFAULT_FG >> 8) & 0xFF,
                  BT_DEFAULT_FG & 0xFF);
  vterm_color_rgb(&def_bg, (BT_DEFAULT_BG >> 16) & 0xFF, (BT_DEFAULT_BG >> 8) & 0xFF,
                  BT_DEFAULT_BG & 0xFF);
  for (guint i = 0; i < 16; i++) {
    guint32 c = bt_theme_ansi(i);
    VTermColor vc;
    vterm_color_rgb(&vc, (uint8_t)((c >> 16) & 0xFFu), (uint8_t)((c >> 8) & 0xFFu),
                    (uint8_t)(c & 0xFFu));
    vterm_state_set_palette_color(bt->state, (int)i, &vc);
  }

  bt->screen = vterm_obtain_screen(bt->vt);
  vterm_screen_set_callbacks(bt->screen, &screen_cbs, bt);
  vterm_screen_enable_altscreen(bt->screen, 1);
  vterm_screen_enable_reflow(bt->screen, true);
  vterm_screen_set_damage_merge(bt->screen, VTERM_DAMAGE_ROW);
  vterm_screen_set_default_colors(bt->screen, &def_fg, &def_bg);
  vterm_screen_reset(bt->screen, 1);
  vterm_screen_flush_damage(bt->screen);

  return bt;
}

void bt_vterm_free(BtVTerm *vt)
{
  if (vt == NULL)
    return;
  cb_sb_clear(vt);
  g_free(vt->sb);
  g_free(vt->grid);
  g_free(vt->title);
  if (vt->vt != NULL)
    vterm_free(vt->vt);
  g_free(vt);
}

void bt_vterm_set_title_cb(BtVTerm *vt, BtVTermTitleFn on_title, gpointer user_data)
{
  g_return_if_fail(vt != NULL);
  vt->on_title = on_title;
  vt->title_ud = user_data;
}

void bt_vterm_feed(BtVTerm *vt, const guint8 *bytes, gsize len)
{
  g_return_if_fail(vt != NULL);
  if (bytes == NULL || len == 0)
    return;
  vterm_input_write(vt->vt, (const char *)bytes, len);
  vterm_screen_flush_damage(vt->screen);
}

void bt_vterm_resize(BtVTerm *vt, gint rows, gint cols)
{
  g_return_if_fail(vt != NULL);
  rows = MAX(rows, 2);
  cols = MAX(cols, 2);
  if (rows == vt->rows && cols == vt->cols)
    return;

  vt->rows = rows;
  vt->cols = cols;
  g_free(vt->grid);
  vt->grid = g_new(BtCell, (gsize)rows * (gsize)cols);
  for (gsize i = 0; i < (gsize)rows * (gsize)cols; i++)
    vt->grid[i] = blank_cell();

  vterm_set_size(vt->vt, rows, cols);
  vterm_screen_flush_damage(vt->screen);

  /* Full-damage refresh after resize (§6.D4 / §14.1). */
  VTermRect full = {0, rows, 0, cols};
  sync_rect_to_grid(vt, full);
}

void bt_vterm_key(BtVTerm *vt, VTermKey key, VTermModifier mod)
{
  g_return_if_fail(vt != NULL);
  vterm_keyboard_key(vt->vt, key, mod);
}

void bt_vterm_unichar(BtVTerm *vt, guint32 codepoint, VTermModifier mod)
{
  g_return_if_fail(vt != NULL);
  vterm_keyboard_unichar(vt->vt, codepoint, mod);
}

void bt_vterm_paste(BtVTerm *vt, const gchar *utf8, gsize len)
{
  g_return_if_fail(vt != NULL);
  if (utf8 == NULL || len == 0)
    return;
  vterm_keyboard_start_paste(vt->vt);
  if (vt->on_output != NULL)
    vt->on_output((const guint8 *)utf8, len, vt->output_ud);
  vterm_keyboard_end_paste(vt->vt);
}

void bt_vterm_mouse_button(BtVTerm *vt, gint button, gboolean pressed, VTermModifier mod)
{
  g_return_if_fail(vt != NULL);
  vterm_mouse_button(vt->vt, button, pressed != FALSE, mod);
}

void bt_vterm_mouse_move(BtVTerm *vt, gint row, gint col, VTermModifier mod)
{
  g_return_if_fail(vt != NULL);
  vterm_mouse_move(vt->vt, row, col, mod);
}

gint bt_vterm_rows(const BtVTerm *vt) { return vt->rows; }
gint bt_vterm_cols(const BtVTerm *vt) { return vt->cols; }
guint bt_vterm_scrollback_len(const BtVTerm *vt) { return vt->sb_len; }
gboolean bt_vterm_is_altscreen(const BtVTerm *vt) { return vt->altscreen; }
gint bt_vterm_mouse_mode(const BtVTerm *vt) { return vt->mouse_mode; }
gboolean bt_vterm_osc52_enabled(const BtVTerm *vt) { return vt->osc52_enabled; }
const gchar *bt_vterm_title(const BtVTerm *vt) { return vt->title ? vt->title : ""; }
guint32 bt_vterm_default_fg(const BtVTerm *vt) { (void)vt; return BT_DEFAULT_FG; }
guint32 bt_vterm_default_bg(const BtVTerm *vt) { (void)vt; return BT_DEFAULT_BG; }

void bt_vterm_cursor(const BtVTerm *vt, gint *row, gint *col, gboolean *visible, gint *shape)
{
  if (row) *row = vt->cursor_row;
  if (col) *col = vt->cursor_col;
  if (visible) *visible = vt->cursor_visible;
  if (shape) *shape = vt->cursor_shape;
}

void bt_vterm_get_vrow_cell(const BtVTerm *vt, guint vrow, gint col, BtCell *out)
{
  if (col < 0) {
    *out = blank_cell();
    return;
  }
  if (vrow < vt->sb_len) {
    guint slot = (vt->sb_head + vrow) % vt->sb_cap;
    const ScrollLine *line = &vt->sb[slot];
    if (col < line->len && line->cells != NULL) {
      *out = line->cells[col];
      return;
    }
    *out = blank_cell();
    return;
  }
  guint srow = vrow - vt->sb_len;
  if ((gint)srow < vt->rows && col < vt->cols) {
    *out = vt->grid[(gsize)srow * (gsize)vt->cols + (gsize)col];
    return;
  }
  *out = blank_cell();
}

gboolean bt_vterm_take_damage(BtVTerm *vt, gint *start_row, gint *end_row)
{
  if (!vt->dirty)
    return FALSE;
  if (start_row) *start_row = vt->dirty_start;
  if (end_row) *end_row = vt->dirty_end;
  vt->dirty = FALSE;
  vt->dirty_start = 0;
  vt->dirty_end = 0;
  return TRUE;
}

static void append_vrow_text(const BtVTerm *vt, guint vrow, gint c0, gint c1, GString *out)
{
  gint max_c = MIN(c1, vt->cols - 1);
  if (vrow < vt->sb_len) {
    guint slot = (vt->sb_head + vrow) % vt->sb_cap;
    max_c = MIN(c1, vt->sb[slot].len - 1);
  }
  gsize start_len = out->len;
  for (gint c = MAX(c0, 0); c <= max_c; c++) {
    BtCell cell;
    bt_vterm_get_vrow_cell(vt, vrow, c, &cell);
    if (cell.width == 0)
      continue;
    if (cell.ch == 0)
      g_string_append_c(out, ' ');
    else
      g_string_append_unichar(out, (gunichar)cell.ch);
  }
  /* Trim trailing spaces on the line. */
  while (out->len > start_len && out->str[out->len - 1] == ' ')
    g_string_truncate(out, out->len - 1);
}

gchar *bt_vterm_extract_text(const BtVTerm *vt, guint start_vrow, gint start_col, guint end_vrow,
                             gint end_col)
{
  g_return_val_if_fail(vt != NULL, NULL);
  if (start_vrow > end_vrow || (start_vrow == end_vrow && start_col > end_col)) {
    guint tr = start_vrow;
    start_vrow = end_vrow;
    end_vrow = tr;
    gint tc = start_col;
    start_col = end_col;
    end_col = tc;
  }
  guint total = vt->sb_len + (guint)vt->rows;
  if (total == 0)
    return g_strdup("");
  end_vrow = MIN(end_vrow, total - 1);

  GString *out = g_string_new(NULL);
  for (guint r = start_vrow; r <= end_vrow; r++) {
    gint c0 = (r == start_vrow) ? start_col : 0;
    gint c1 = (r == end_vrow) ? end_col : vt->cols - 1;
    append_vrow_text(vt, r, c0, c1, out);
    if (r < end_vrow)
      g_string_append_c(out, '\n');
  }
  return g_string_free(out, FALSE);
}

GArray *bt_vterm_search(const BtVTerm *vt, const gchar *query)
{
  GArray *hits = g_array_new(FALSE, FALSE, sizeof(guint));
  if (vt == NULL || query == NULL || query[0] == '\0')
    return hits;
  gchar *needle = g_utf8_casefold(query, -1);
  guint total = vt->sb_len + (guint)vt->rows;
  GString *line = g_string_sized_new(128);

  for (guint r = 0; r < total; r++) {
    g_string_truncate(line, 0);
    append_vrow_text(vt, r, 0, vt->cols - 1, line);
    if (line->len == 0)
      continue;
    gchar *folded = g_utf8_casefold(line->str, (gssize)line->len);
    if (strstr(folded, needle) != NULL)
      g_array_append_val(hits, r);
    g_free(folded);
  }
  g_string_free(line, TRUE);
  g_free(needle);
  return hits;
}
