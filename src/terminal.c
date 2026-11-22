/* terminal.c — Custom Pango/Cairo grid renderer + ConPTY/openpty + libvterm (§5.4, §6.D). */
#include "terminal.h"
#include "pty.h"
#include "theme.h"
#include "ui/themeload.h"
#include "vtermbridge.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define BT_TERM_RESIZE_DEBOUNCE_MS 100u
#define BT_TERM_PAD                12.0

struct BtTerminal {
  GtkWidget *root;
  GtkWidget *search_bar;
  GtkWidget *search_entry;
  GtkWidget *search_count_label;
  GtkWidget *info_bar;
  GtkWidget *info_label;
  GtkWidget *drawing_area;
  GtkWidget *ctx_popover;
  GtkWidget *scrollbar;
  GtkAdjustment *vadj;

  GtkIMContext *im_ctx;
  PangoFontDescription *font_desc;
  PangoFontDescription *font_bold;
  PangoFontDescription *font_italic;
  PangoFontDescription *font_bold_italic;
  gdouble cell_w;
  gdouble cell_h;
  gdouble baseline;

  BtVTerm *vt;
  BtPty *pty;
  gchar *cwd;

  /* Viewport: lines scrolled up from bottom (0 = live bottom). */
  guint scroll_offset;
  gboolean updating_adj;

  /* Resize debounce (§5.4). */
  guint resize_timer_id;
  gint pending_rows;
  gint pending_cols;

  /* Mouse selection in virtual coordinates. */
  gboolean selecting;
  gboolean has_selection;
  guint sel_start_vrow;
  gint sel_start_col;
  guint sel_end_vrow;
  gint sel_end_col;

  /* Scrollback search (Ctrl+Shift+F). */
  GArray *search_hits;
  gint search_cur;

  BtTerminalFullscreenFn on_fullscreen;
  BtTerminalStatusFn on_status;
  gpointer user_data;
  BtTerminalExitFn on_exit_cb;
  gpointer exit_ud;
};

static inline void set_rgb(cairo_t *cr, guint32 rgb)
{
  cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                       (rgb & 0xFF) / 255.0);
}

static void sync_scrollbar(BtTerminal *t)
{
  if (t->vt == NULL)
    return;
  guint sb = bt_vterm_scrollback_len(t->vt);
  gint rows = bt_vterm_rows(t->vt);
  if (t->scroll_offset > sb)
    t->scroll_offset = sb;

  t->updating_adj = TRUE;
  gtk_adjustment_configure(t->vadj, (gdouble)(sb - t->scroll_offset), 0.0,
                           (gdouble)(sb + (guint)rows), 1.0, (gdouble)MAX(rows - 1, 1),
                           (gdouble)rows);
  t->updating_adj = FALSE;
}

static void show_banner(BtTerminal *t, const gchar *msg)
{
  gtk_label_set_text(GTK_LABEL(t->info_label), msg);
  gtk_widget_set_visible(t->info_bar, TRUE);
}

/* ----------------------------------------------------- PTY & VTerm glue -- */

static void on_vterm_output(const guint8 *bytes, gsize len, gpointer ud)
{
  BtTerminal *t = ud;
  if (t->pty != NULL)
    bt_pty_write(t->pty, bytes, len);
}

static void on_pty_data(const guint8 *data, gsize len, gpointer ud)
{
  BtTerminal *t = ud;
  if (t->vt == NULL)
    return;
  guint sb_before = bt_vterm_scrollback_len(t->vt);
  bt_vterm_feed(t->vt, data, len);
  guint sb_after = bt_vterm_scrollback_len(t->vt);
  if (t->scroll_offset > 0 && sb_after > sb_before)
    t->scroll_offset += (sb_after - sb_before);

  gint r0 = 0, r1 = 0;
  if (bt_vterm_take_damage(t->vt, &r0, &r1) || sb_after != sb_before) {
    sync_scrollbar(t);
    gtk_widget_queue_draw(t->drawing_area);
  }
}

static void on_pty_exit(gint exit_code, gpointer ud)
{
  BtTerminal *t = ud;
  gchar *msg = g_strdup_printf("Process exited with status %d", exit_code);
  show_banner(t, msg);
  if (t->on_status != NULL)
    t->on_status(msg, t->user_data);
  if (t->on_exit_cb != NULL)
    t->on_exit_cb(t, exit_code, t->exit_ud);
  g_free(msg);
  gtk_widget_queue_draw(t->drawing_area);
}

static void on_pty_backpressure(gboolean dropped, gpointer ud)
{
  BtTerminal *t = ud;
  if (dropped && t->on_status != NULL)
    t->on_status("Terminal write queue full (1 MB cap): input dropped", t->user_data);
}

/* --------------------------------------------------------- font metrics -- */

static void ensure_font_metrics(BtTerminal *t, cairo_t *cr)
{
  if (t->cell_w > 0.0)
    return;
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, t->font_desc);
  pango_layout_set_text(layout, "MMMMMMMMMM", 10);
  int w = 0, h = 0;
  pango_layout_get_pixel_size(layout, &w, &h);
  t->cell_w = MAX((gdouble)w / 10.0, 6.0);
  t->cell_h = MAX((gdouble)h, 12.0);
  t->baseline = (gdouble)pango_layout_get_baseline(layout) / (gdouble)PANGO_SCALE;
  g_object_unref(layout);
}

static gboolean is_selected(const BtTerminal *t, guint vrow, gint col)
{
  if (!t->has_selection)
    return FALSE;
  guint r0 = t->sel_start_vrow, r1 = t->sel_end_vrow;
  gint c0 = t->sel_start_col, c1 = t->sel_end_col;
  if (r0 > r1 || (r0 == r1 && c0 > c1)) {
    guint tr = r0; r0 = r1; r1 = tr;
    gint tc = c0; c0 = c1; c1 = tc;
  }
  if (vrow < r0 || vrow > r1)
    return FALSE;
  if (r0 == r1)
    return col >= c0 && col <= c1;
  if (vrow == r0)
    return col >= c0;
  if (vrow == r1)
    return col <= c1;
  return TRUE;
}

static gboolean is_search_hit_row(const BtTerminal *t, guint vrow)
{
  if (t->search_hits == NULL || t->search_hits->len == 0)
    return FALSE;
  for (guint i = 0; i < t->search_hits->len; i++)
    if (g_array_index(t->search_hits, guint, i) == vrow)
      return TRUE;
  return FALSE;
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
  (void)area;
  BtTerminal *t = ud;
  ensure_font_metrics(t, cr);

  guint32 def_bg = bt_vterm_default_bg(t->vt);
  set_rgb(cr, def_bg);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (t->vt == NULL)
    return;

  gint rows = bt_vterm_rows(t->vt);
  gint cols = bt_vterm_cols(t->vt);
  guint sb = bt_vterm_scrollback_len(t->vt);
  guint top_vrow = (sb >= t->scroll_offset) ? (sb - t->scroll_offset) : 0;

  guint32 sel_bg = bt_theme_terminal_selection();
  guint32 hit_bg = bt_theme_terminal_search_hit();

  PangoLayout *layout = pango_cairo_create_layout(cr);
  GString *run = g_string_sized_new(128);

  for (gint r = 0; r < rows; r++) {
    guint vrow = top_vrow + (guint)r;
    gdouble y = BT_TERM_PAD + (gdouble)r * t->cell_h;
    if (y > (gdouble)height)
      break;

    gboolean hit_row = is_search_hit_row(t, vrow);

    /* Pass 1: backgrounds (coalesced horizontal spans). */
    gint c = 0;
    while (c < cols) {
      BtCell cell;
      bt_vterm_get_vrow_cell(t->vt, vrow, c, &cell);
      guint32 bg = (cell.flags & BT_CELL_REVERSE) ? cell.fg_rgb : cell.bg_rgb;
      if (is_selected(t, vrow, c))
        bg = sel_bg;
      else if (hit_row && bg == def_bg)
        bg = hit_bg;

      gint run_end = c + 1;
      while (run_end < cols) {
        BtCell nxt;
        bt_vterm_get_vrow_cell(t->vt, vrow, run_end, &nxt);
        guint32 nbg = (nxt.flags & BT_CELL_REVERSE) ? nxt.fg_rgb : nxt.bg_rgb;
        if (is_selected(t, vrow, run_end))
          nbg = sel_bg;
        else if (hit_row && nbg == def_bg)
          nbg = hit_bg;
        if (nbg != bg)
          break;
        run_end++;
      }
      if (bg != def_bg) {
        set_rgb(cr, bg);
        cairo_rectangle(cr, BT_TERM_PAD + (gdouble)c * t->cell_w, y,
                        (gdouble)(run_end - c) * t->cell_w, t->cell_h);
        cairo_fill(cr);
      }
      c = run_end;
    }

    /* Pass 2: foreground glyphs (coalesced style runs). */
    c = 0;
    while (c < cols) {
      BtCell cell;
      bt_vterm_get_vrow_cell(t->vt, vrow, c, &cell);
      if (cell.width == 0 || cell.ch == 0 || cell.ch == ' ' || (cell.flags & BT_CELL_CONCEAL)) {
        c++;
        continue;
      }
      guint32 fg = (cell.flags & BT_CELL_REVERSE) ? cell.bg_rgb : cell.fg_rgb;
      guint8 style_flags = cell.flags & (BT_CELL_BOLD | BT_CELL_ITALIC | BT_CELL_UNDERLINE |
                                         BT_CELL_STRIKE);
      gint start_c = c;
      g_string_truncate(run, 0);

      while (c < cols) {
        BtCell cur;
        bt_vterm_get_vrow_cell(t->vt, vrow, c, &cur);
        if (cur.width == 0) {
          c++;
          continue;
        }
        guint32 cfg = (cur.flags & BT_CELL_REVERSE) ? cur.bg_rgb : cur.fg_rgb;
        guint8 cstyle = cur.flags & (BT_CELL_BOLD | BT_CELL_ITALIC | BT_CELL_UNDERLINE |
                                     BT_CELL_STRIKE);
        if (cfg != fg || cstyle != style_flags || (cur.flags & BT_CELL_CONCEAL))
          break;
        g_string_append_unichar(run, cur.ch ? (gunichar)cur.ch : ' ');
        c += MAX(cur.width, 1);
        /* Stop coalescing across wide chars to keep cell grid alignment exact. */
        if (cur.width > 1)
          break;
      }

      PangoFontDescription *fd = t->font_desc;
      if ((style_flags & BT_CELL_BOLD) && (style_flags & BT_CELL_ITALIC))
        fd = t->font_bold_italic;
      else if (style_flags & BT_CELL_BOLD)
        fd = t->font_bold;
      else if (style_flags & BT_CELL_ITALIC)
        fd = t->font_italic;

      pango_layout_set_font_description(layout, fd);
      pango_layout_set_text(layout, run->str, (int)run->len);
      set_rgb(cr, fg);
      cairo_move_to(cr, BT_TERM_PAD + (gdouble)start_c * t->cell_w, y);
      pango_cairo_show_layout(cr, layout);

      gdouble run_px = (gdouble)(c - start_c) * t->cell_w;
      if (style_flags & BT_CELL_UNDERLINE) {
        cairo_rectangle(cr, BT_TERM_PAD + (gdouble)start_c * t->cell_w, y + t->cell_h - 2.0,
                        run_px, 1.0);
        cairo_fill(cr);
      }
      if (style_flags & BT_CELL_STRIKE) {
        cairo_rectangle(cr, BT_TERM_PAD + (gdouble)start_c * t->cell_w, y + t->cell_h * 0.5,
                        run_px, 1.0);
        cairo_fill(cr);
      }
    }
  }

  /* Pass 3: accent cursor (only when viewing live bottom). */
  gint cur_r = 0, cur_c = 0, cur_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
  gboolean cur_vis = FALSE;
  bt_vterm_cursor(t->vt, &cur_r, &cur_c, &cur_vis, &cur_shape);
  if (cur_vis && t->scroll_offset == 0 && cur_r >= 0 && cur_r < rows && cur_c >= 0 &&
      cur_c < cols) {
    gdouble cx = BT_TERM_PAD + (gdouble)cur_c * t->cell_w;
    gdouble cy = BT_TERM_PAD + (gdouble)cur_r * t->cell_h;
    gdouble ar, ag, ab;
    bt_theme_unpack(BT_COL_ACCENT, &ar, &ag, &ab);
    cairo_set_source_rgba(cr, ar, ag, ab, 0.85);
    if (cur_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE)
      cairo_rectangle(cr, cx, cy + t->cell_h - 2.0, t->cell_w, 2.0);
    else if (cur_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT)
      cairo_rectangle(cr, cx, cy, 2.0, t->cell_h);
    else
      cairo_rectangle(cr, cx, cy, t->cell_w, t->cell_h);
    cairo_fill(cr);
  }

  g_string_free(run, TRUE);
  g_object_unref(layout);
}

/* ------------------------------------------------ debounced resize (§5.4) */

static gboolean on_resize_timeout(gpointer ud)
{
  BtTerminal *t = ud;
  t->resize_timer_id = 0;
  if (t->pending_rows >= 2 && t->pending_cols >= 2) {
    if (t->pty != NULL)
      bt_pty_resize(t->pty, t->pending_rows, t->pending_cols, NULL);
    if (t->vt != NULL)
      bt_vterm_resize(t->vt, t->pending_rows, t->pending_cols);
    sync_scrollbar(t);
    gtk_widget_queue_draw(t->drawing_area);
  }
  return G_SOURCE_REMOVE;
}

static void on_area_resize(GtkDrawingArea *area, int width, int height, gpointer ud)
{
  (void)area;
  BtTerminal *t = ud;
  gdouble cw = t->cell_w > 0.0 ? t->cell_w : 8.0;
  gdouble ch = t->cell_h > 0.0 ? t->cell_h : 18.0;
  gdouble inner_w = MAX((gdouble)width - BT_TERM_PAD * 2.0, cw * 2.0);
  gdouble inner_h = MAX((gdouble)height - BT_TERM_PAD * 2.0, ch * 2.0);
  gint cols = MAX((gint)floor(inner_w / cw), 2);
  gint rows = MAX((gint)floor(inner_h / ch), 2);
  if (t->vt != NULL && rows == bt_vterm_rows(t->vt) && cols == bt_vterm_cols(t->vt))
    return;
  t->pending_rows = rows;
  t->pending_cols = cols;
  if (t->resize_timer_id != 0)
    g_source_remove(t->resize_timer_id);
  t->resize_timer_id = g_timeout_add(BT_TERM_RESIZE_DEBOUNCE_MS, on_resize_timeout, t);
}

/* ------------------------------------------- clipboard & scrollback search */

static void copy_selection(BtTerminal *t)
{
  if (!t->has_selection || t->vt == NULL)
    return;
  gchar *text = bt_vterm_extract_text(t->vt, t->sel_start_vrow, t->sel_start_col, t->sel_end_vrow,
                                      t->sel_end_col);
  if (text != NULL && text[0] != '\0') {
    GdkClipboard *cb = gtk_widget_get_clipboard(t->drawing_area);
    gdk_clipboard_set_text(cb, text);
  }
  g_free(text);
}

static void on_paste_ready(GObject *src, GAsyncResult *res, gpointer ud)
{
  BtTerminal *t = ud;
  gchar *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
  if (text != NULL && t->vt != NULL) {
    t->scroll_offset = 0;
    bt_vterm_paste(t->vt, text, strlen(text));
    sync_scrollbar(t);
    gtk_widget_queue_draw(t->drawing_area);
  }
  g_free(text);
}

static void paste_clipboard(BtTerminal *t)
{
  GdkClipboard *cb = gtk_widget_get_clipboard(t->drawing_area);
  gdk_clipboard_read_text_async(cb, NULL, on_paste_ready, t);
}

static void jump_to_search_hit(BtTerminal *t, gint idx)
{
  if (t->search_hits == NULL || t->search_hits->len == 0 || t->vt == NULL)
    return;
  gint n = (gint)t->search_hits->len;
  idx = ((idx % n) + n) % n;
  t->search_cur = idx;
  guint vrow = g_array_index(t->search_hits, guint, (guint)idx);
  guint sb = bt_vterm_scrollback_len(t->vt);
  if (vrow < sb)
    t->scroll_offset = sb - vrow;
  else
    t->scroll_offset = 0;
  sync_scrollbar(t);
  gchar *lbl = g_strdup_printf("%d / %d", idx + 1, n);
  gtk_label_set_text(GTK_LABEL(t->search_count_label), lbl);
  g_free(lbl);
  gtk_widget_queue_draw(t->drawing_area);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer ud)
{
  BtTerminal *t = ud;
  const gchar *q = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (t->search_hits != NULL)
    g_array_unref(t->search_hits);
  t->search_hits = bt_vterm_search(t->vt, q);
  t->search_cur = 0;
  if (t->search_hits->len > 0) {
    jump_to_search_hit(t, (gint)t->search_hits->len - 1);
  } else {
    gtk_label_set_text(GTK_LABEL(t->search_count_label), q[0] ? "0 / 0" : "");
    gtk_widget_queue_draw(t->drawing_area);
  }
}

static void on_search_activate(GtkSearchEntry *entry, gpointer ud)
{
  (void)entry;
  BtTerminal *t = ud;
  jump_to_search_hit(t, t->search_cur - 1);
}

static void on_search_close(GtkButton *btn, gpointer ud)
{
  (void)btn;
  BtTerminal *t = ud;
  gtk_widget_set_visible(t->search_bar, FALSE);
  if (t->search_hits != NULL) {
    g_array_unref(t->search_hits);
    t->search_hits = NULL;
  }
  gtk_widget_queue_draw(t->drawing_area);
  gtk_widget_grab_focus(t->drawing_area);
}

/* ------------------------------------------- keymap (Appendix A) & IME -- */

static void on_im_commit(GtkIMContext *ctx, const gchar *str, gpointer ud)
{
  (void)ctx;
  BtTerminal *t = ud;
  if (t->pty == NULL || str == NULL || str[0] == '\0')
    return;
  t->scroll_offset = 0;
  bt_pty_write(t->pty, (const guint8 *)str, strlen(str));
  sync_scrollbar(t);
}

static VTermModifier gdk_to_vterm_mod(GdkModifierType state)
{
  VTermModifier mod = VTERM_MOD_NONE;
  if (state & GDK_SHIFT_MASK)
    mod |= VTERM_MOD_SHIFT;
  if (state & GDK_ALT_MASK)
    mod |= VTERM_MOD_ALT;
  if (state & GDK_CONTROL_MASK)
    mod |= VTERM_MOD_CTRL;
  return mod;
}

static VTermKey map_special_key(guint keyval)
{
  switch (keyval) {
  case GDK_KEY_Return: case GDK_KEY_KP_Enter: return VTERM_KEY_ENTER;
  case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return VTERM_KEY_TAB;
  case GDK_KEY_BackSpace: return VTERM_KEY_BACKSPACE;
  case GDK_KEY_Escape: return VTERM_KEY_ESCAPE;
  case GDK_KEY_Up: return VTERM_KEY_UP;
  case GDK_KEY_Down: return VTERM_KEY_DOWN;
  case GDK_KEY_Left: return VTERM_KEY_LEFT;
  case GDK_KEY_Right: return VTERM_KEY_RIGHT;
  case GDK_KEY_Insert: return VTERM_KEY_INS;
  case GDK_KEY_Delete: return VTERM_KEY_DEL;
  case GDK_KEY_Home: return VTERM_KEY_HOME;
  case GDK_KEY_End: return VTERM_KEY_END;
  case GDK_KEY_Page_Up: return VTERM_KEY_PAGEUP;
  case GDK_KEY_Page_Down: return VTERM_KEY_PAGEDOWN;
  default:
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)
      return VTERM_KEY_FUNCTION((int)(keyval - GDK_KEY_F1 + 1));
    return VTERM_KEY_NONE;
  }
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                               GdkModifierType state, gpointer ud)
{
  (void)keycode;
  BtTerminal *t = ud;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;
  gboolean mod_ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean alt = (state & GDK_ALT_MASK) != 0;

  /* Intercepted BrainTerm terminal shortcuts (Appendix A). */
  if (shift && !mod_ctrl && !alt && (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down)) {
    guint step = (guint)MAX(bt_vterm_rows(t->vt) / 2, 1);
    guint sb = bt_vterm_scrollback_len(t->vt);
    if (keyval == GDK_KEY_Page_Up)
      t->scroll_offset = MIN(t->scroll_offset + step, sb);
    else
      t->scroll_offset = (t->scroll_offset > step) ? (t->scroll_offset - step) : 0;
    sync_scrollbar(t);
    gtk_widget_queue_draw(t->drawing_area);
    return TRUE;
  }
  if (mod_ctrl && shift && (keyval == GDK_KEY_C || keyval == GDK_KEY_c)) {
    copy_selection(t);
    return TRUE;
  }
  if (mod_ctrl && shift && (keyval == GDK_KEY_V || keyval == GDK_KEY_v)) {
    paste_clipboard(t);
    return TRUE;
  }
  if (mod_ctrl && shift && (keyval == GDK_KEY_F || keyval == GDK_KEY_f)) {
    gboolean vis = !gtk_widget_get_visible(t->search_bar);
    gtk_widget_set_visible(t->search_bar, vis);
    if (vis)
      gtk_widget_grab_focus(t->search_entry);
    else
      gtk_widget_grab_focus(t->drawing_area);
    return TRUE;
  }
  if (alt && !mod_ctrl && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
    if (t->on_fullscreen != NULL)
      t->on_fullscreen(t->user_data);
    return TRUE;
  }

  if (t->vt == NULL || t->pty == NULL)
    return FALSE;

  /* Ignore pure modifier key presses. */
  if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R || keyval == GDK_KEY_Control_L ||
      keyval == GDK_KEY_Control_R || keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
      keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R)
    return FALSE;

  t->scroll_offset = 0;
  t->has_selection = FALSE;

  VTermKey vk = map_special_key(keyval);
  if (vk != VTERM_KEY_NONE) {
    bt_vterm_key(t->vt, vk, gdk_to_vterm_mod(state));
    sync_scrollbar(t);
    return TRUE;
  }

  /* Let IME handle printable non-Ctrl/Alt input for CJK/dead keys. */
  if (!mod_ctrl && !alt) {
    GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctrl));
    if (ev != NULL && gtk_im_context_filter_keypress(t->im_ctx, ev))
      return TRUE;
  }

  gunichar ch = gdk_keyval_to_unicode(keyval);
  if (ch != 0) {
    VTermModifier mod = gdk_to_vterm_mod(state);
    if (!mod_ctrl && !alt)
      mod &= ~VTERM_MOD_SHIFT;
    bt_vterm_unichar(t->vt, ch, mod);
    sync_scrollbar(t);
    return TRUE;
  }
  return FALSE;
}

/* ----------------------------------------------------- mouse & scroll -- */

static void xy_to_cell(BtTerminal *t, gdouble x, gdouble y, guint *vrow, gint *col, gint *srow)
{
  gdouble cw = t->cell_w > 0.0 ? t->cell_w : 8.0;
  gdouble ch = t->cell_h > 0.0 ? t->cell_h : 18.0;
  gint r = CLAMP((gint)((y - BT_TERM_PAD) / ch), 0, bt_vterm_rows(t->vt) - 1);
  gint c = CLAMP((gint)((x - BT_TERM_PAD) / cw), 0, bt_vterm_cols(t->vt) - 1);
  guint sb = bt_vterm_scrollback_len(t->vt);
  guint top = (sb >= t->scroll_offset) ? (sb - t->scroll_offset) : 0;
  if (srow) *srow = r;
  if (vrow) *vrow = top + (guint)r;
  if (col) *col = c;
}

static void on_mouse_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)n_press;
  BtTerminal *t = ud;
  gtk_widget_grab_focus(t->drawing_area);
  if (t->vt == NULL)
    return;
  guint vrow = 0;
  gint col = 0, srow = 0;
  xy_to_cell(t, x, y, &vrow, &col, &srow);

  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
  GdkEvent *ev = gtk_gesture_get_last_event(GTK_GESTURE(g), seq);
  GdkModifierType st = ev ? gdk_event_get_modifier_state(ev) : 0;

  if (bt_vterm_mouse_mode(t->vt) != VTERM_PROP_MOUSE_NONE && (st & GDK_SHIFT_MASK) == 0) {
    bt_vterm_mouse_move(t->vt, srow, col, gdk_to_vterm_mod(st));
    bt_vterm_mouse_button(t->vt, 1, TRUE, gdk_to_vterm_mod(st));
    return;
  }

  t->selecting = TRUE;
  t->has_selection = FALSE;
  t->sel_start_vrow = t->sel_end_vrow = vrow;
  t->sel_start_col = t->sel_end_col = col;
  gtk_widget_queue_draw(t->drawing_area);
}

static void on_mouse_released(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  (void)n_press;
  BtTerminal *t = ud;
  if (t->vt == NULL)
    return;
  guint vrow = 0;
  gint col = 0, srow = 0;
  xy_to_cell(t, x, y, &vrow, &col, &srow);

  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
  GdkEvent *ev = gtk_gesture_get_last_event(GTK_GESTURE(g), seq);
  GdkModifierType st = ev ? gdk_event_get_modifier_state(ev) : 0;

  if (bt_vterm_mouse_mode(t->vt) != VTERM_PROP_MOUSE_NONE && (st & GDK_SHIFT_MASK) == 0) {
    bt_vterm_mouse_button(t->vt, 1, FALSE, gdk_to_vterm_mod(st));
    return;
  }
  t->selecting = FALSE;
}

static void on_mouse_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer ud)
{
  (void)ctrl;
  BtTerminal *t = ud;
  if (!t->selecting || t->vt == NULL)
    return;
  xy_to_cell(t, x, y, &t->sel_end_vrow, &t->sel_end_col, NULL);
  t->has_selection = (t->sel_start_vrow != t->sel_end_vrow || t->sel_start_col != t->sel_end_col);
  gtk_widget_queue_draw(t->drawing_area);
}

static gboolean on_scroll(GtkEventControllerScroll *ctrl, gdouble dx, gdouble dy, gpointer ud)
{
  (void)ctrl;
  (void)dx;
  BtTerminal *t = ud;
  if (t->vt == NULL)
    return FALSE;
  if (bt_vterm_mouse_mode(t->vt) != VTERM_PROP_MOUSE_NONE) {
    gint btn = dy < 0 ? 4 : 5;
    bt_vterm_mouse_button(t->vt, btn, TRUE, VTERM_MOD_NONE);
    return TRUE;
  }
  guint sb = bt_vterm_scrollback_len(t->vt);
  gint delta = (gint)round(dy * 3.0);
  if (delta < 0)
    t->scroll_offset = MIN(t->scroll_offset + (guint)(-delta), sb);
  else if (delta > 0)
    t->scroll_offset = (t->scroll_offset > (guint)delta) ? (t->scroll_offset - (guint)delta) : 0;
  sync_scrollbar(t);
  gtk_widget_queue_draw(t->drawing_area);
  return TRUE;
}

static void on_vadj_changed(GtkAdjustment *adj, gpointer ud)
{
  BtTerminal *t = ud;
  if (t->updating_adj || t->vt == NULL)
    return;
  guint sb = bt_vterm_scrollback_len(t->vt);
  guint val = (guint)CLAMP(gtk_adjustment_get_value(adj), 0.0, (gdouble)sb);
  t->scroll_offset = sb - val;
  gtk_widget_queue_draw(t->drawing_area);
}

static void on_reopen_clicked(GtkButton *btn, gpointer ud)
{
  (void)btn;
  BtTerminal *t = ud;
  if (t->cwd != NULL) {
    GError *err = NULL;
    if (!bt_terminal_spawn(t, t->cwd, &err)) {
      show_banner(t, err->message);
      g_error_free(err);
    }
  }
}

static void on_area_realize(GtkWidget *w, gpointer ud)
{
  BtTerminal *t = ud;
  gtk_im_context_set_client_widget(t->im_ctx, w);
}

static void on_area_unrealize(GtkWidget *w, gpointer ud)
{
  (void)w;
  BtTerminal *t = ud;
  gtk_im_context_set_client_widget(t->im_ctx, NULL);
}

static void on_term_ctx_copy(GtkButton *b, gpointer ud)
{
  (void)b;
  BtTerminal *t = ud;
  if (t->ctx_popover != NULL)
    gtk_popover_popdown(GTK_POPOVER(t->ctx_popover));
  copy_selection(t);
}

static void on_term_ctx_paste(GtkButton *b, gpointer ud)
{
  (void)b;
  BtTerminal *t = ud;
  if (t->ctx_popover != NULL)
    gtk_popover_popdown(GTK_POPOVER(t->ctx_popover));
  paste_clipboard(t);
  gtk_widget_grab_focus(t->drawing_area);
}

static void on_term_ctx_find(GtkButton *b, gpointer ud)
{
  (void)b;
  BtTerminal *t = ud;
  if (t->ctx_popover != NULL)
    gtk_popover_popdown(GTK_POPOVER(t->ctx_popover));
  gtk_widget_set_visible(t->search_bar, TRUE);
  gtk_widget_grab_focus(t->search_entry);
}

static void on_term_ctx_clear(GtkButton *b, gpointer ud)
{
  (void)b;
  BtTerminal *t = ud;
  if (t->ctx_popover != NULL)
    gtk_popover_popdown(GTK_POPOVER(t->ctx_popover));
  if (t->pty != NULL)
    bt_pty_write(t->pty, (const guint8 *)"\x0c", 1);
  gtk_widget_grab_focus(t->drawing_area);
}

static void on_term_ctx_closed(GtkPopover *pop, gpointer ud)
{
  BtTerminal *t = ud;
  if (t->ctx_popover == GTK_WIDGET(pop)) {
    t->ctx_popover = NULL;
    gtk_widget_unparent(GTK_WIDGET(pop));
  }
}

static GtkWidget *make_term_ctx_btn(const gchar *label, GCallback cb, gpointer ud, gboolean sens)
{
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_add_css_class(b, "bt-ctx-item");
  GtkWidget *ch = gtk_button_get_child(GTK_BUTTON(b));
  if (GTK_IS_LABEL(ch))
    gtk_label_set_xalign(GTK_LABEL(ch), 0.0f);
  gtk_widget_set_sensitive(b, sens);
  g_signal_connect(b, "clicked", cb, ud);
  return b;
}

static void on_term_right_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer ud)
{
  if (n_press != 1)
    return;
  BtTerminal *t = ud;
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
  if (t->ctx_popover != NULL) {
    GtkWidget *old = t->ctx_popover;
    t->ctx_popover = NULL;
    gtk_popover_popdown(GTK_POPOVER(old));
    gtk_widget_unparent(old);
  }
  GtkWidget *pop = gtk_popover_new();
  gtk_widget_add_css_class(pop, "bt-ctx-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
  gtk_widget_set_halign(pop, GTK_ALIGN_START);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "bt-ctx-menu");
  gtk_box_append(GTK_BOX(box), make_term_ctx_btn("Copy   Ctrl+Shift+C",
                                                 G_CALLBACK(on_term_ctx_copy), t, t->has_selection));
  gtk_box_append(GTK_BOX(box), make_term_ctx_btn("Paste   Ctrl+Shift+V",
                                                 G_CALLBACK(on_term_ctx_paste), t, t->pty != NULL));
  gtk_box_append(GTK_BOX(box), make_term_ctx_btn("Find…   Ctrl+Shift+F",
                                                 G_CALLBACK(on_term_ctx_find), t, TRUE));
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(sep, "bt-ctx-sep");
  gtk_box_append(GTK_BOX(box), sep);
  gtk_box_append(GTK_BOX(box), make_term_ctx_btn("Clear   Ctrl+L",
                                                 G_CALLBACK(on_term_ctx_clear), t, t->pty != NULL));

  gtk_popover_set_child(GTK_POPOVER(pop), box);
  gtk_widget_set_parent(pop, t->drawing_area);
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  g_signal_connect(pop, "closed", G_CALLBACK(on_term_ctx_closed), t);
  t->ctx_popover = pop;
  gtk_popover_popup(GTK_POPOVER(pop));
}

/* ---------------------------------------------------------- public API -- */

BtTerminal *bt_terminal_new(BtTerminalFullscreenFn on_fullscreen, BtTerminalStatusFn on_status,
                            gpointer user_data)
{
  BtTerminal *t = g_new0(BtTerminal, 1);
  t->on_fullscreen = on_fullscreen;
  t->on_status = on_status;
  t->user_data = user_data;

  const gchar *fam = bt_fonts_family_available("JetBrains Mono") ? "JetBrains Mono"
                     : (bt_fonts_family_available("Cascadia Mono") ? "Cascadia Mono"
                        : (bt_fonts_family_available("Consolas") ? "Consolas" : "Monospace"));
  t->font_desc = pango_font_description_new();
  pango_font_description_set_family(t->font_desc, fam);
  pango_font_description_set_absolute_size(t->font_desc, 13 * PANGO_SCALE);
  t->font_bold = pango_font_description_copy(t->font_desc);
  pango_font_description_set_weight(t->font_bold, PANGO_WEIGHT_BOLD);
  t->font_italic = pango_font_description_copy(t->font_desc);
  pango_font_description_set_style(t->font_italic, PANGO_STYLE_ITALIC);
  t->font_bold_italic = pango_font_description_copy(t->font_bold);
  pango_font_description_set_style(t->font_bold_italic, PANGO_STYLE_ITALIC);

  t->vt = bt_vterm_new(24, 80, BT_SCROLLBACK_DEFAULT, on_vterm_output, t);

  t->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* Search bar (Ctrl+Shift+F) */
  t->search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(t->search_bar, "bt-banner");
  gtk_widget_set_margin_start(t->search_bar, 6);
  gtk_widget_set_margin_end(t->search_bar, 6);
  gtk_widget_set_margin_top(t->search_bar, 4);
  gtk_widget_set_margin_bottom(t->search_bar, 4);
  t->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(t->search_entry, TRUE);
  t->search_count_label = gtk_label_new("");
  GtkWidget *close_s = gtk_button_new_with_label("×");
  g_signal_connect(t->search_entry, "search-changed", G_CALLBACK(on_search_changed), t);
  g_signal_connect(t->search_entry, "activate", G_CALLBACK(on_search_activate), t);
  g_signal_connect(close_s, "clicked", G_CALLBACK(on_search_close), t);
  gtk_box_append(GTK_BOX(t->search_bar), t->search_entry);
  gtk_box_append(GTK_BOX(t->search_bar), t->search_count_label);
  gtk_box_append(GTK_BOX(t->search_bar), close_s);
  gtk_widget_set_visible(t->search_bar, FALSE);
  gtk_box_append(GTK_BOX(t->root), t->search_bar);

  /* Exit / error banner (R5) */
  t->info_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(t->info_bar, "bt-banner");
  gtk_widget_set_margin_start(t->info_bar, 8);
  gtk_widget_set_margin_end(t->info_bar, 8);
  gtk_widget_set_margin_top(t->info_bar, 4);
  gtk_widget_set_margin_bottom(t->info_bar, 4);
  t->info_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(t->info_label), 0.0f);
  gtk_widget_set_hexpand(t->info_label, TRUE);
  GtkWidget *reopen_btn = gtk_button_new_with_label("Reopen Shell");
  g_signal_connect(reopen_btn, "clicked", G_CALLBACK(on_reopen_clicked), t);
  gtk_box_append(GTK_BOX(t->info_bar), t->info_label);
  gtk_box_append(GTK_BOX(t->info_bar), reopen_btn);
  gtk_widget_set_visible(t->info_bar, FALSE);
  gtk_box_append(GTK_BOX(t->root), t->info_bar);

  /* Canvas + scrollbar */
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand(hbox, TRUE);
  gtk_widget_set_hexpand(hbox, TRUE);

  t->drawing_area = gtk_drawing_area_new();
  gtk_widget_add_css_class(t->drawing_area, "bt-terminal-area");
  gtk_widget_set_hexpand(t->drawing_area, TRUE);
  gtk_widget_set_vexpand(t->drawing_area, TRUE);
  gtk_widget_set_focusable(t->drawing_area, TRUE);
  gtk_widget_set_size_request(t->drawing_area, 200, 140);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(t->drawing_area), on_draw, t, NULL);
  g_signal_connect(t->drawing_area, "resize", G_CALLBACK(on_area_resize), t);

  t->im_ctx = gtk_im_multicontext_new();
  g_signal_connect(t->drawing_area, "realize", G_CALLBACK(on_area_realize), t);
  g_signal_connect(t->drawing_area, "unrealize", G_CALLBACK(on_area_unrealize), t);
  g_signal_connect(t->im_ctx, "commit", G_CALLBACK(on_im_commit), t);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), t);
  gtk_widget_add_controller(t->drawing_area, key);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_mouse_pressed), t);
  g_signal_connect(click, "released", G_CALLBACK(on_mouse_released), t);
  gtk_widget_add_controller(t->drawing_area, GTK_EVENT_CONTROLLER(click));

  GtkGesture *rclick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect(rclick, "pressed", G_CALLBACK(on_term_right_click), t);
  gtk_widget_add_controller(t->drawing_area, GTK_EVENT_CONTROLLER(rclick));

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_mouse_motion), t);
  gtk_widget_add_controller(t->drawing_area, motion);

  GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), t);
  gtk_widget_add_controller(t->drawing_area, scroll);

  t->vadj = gtk_adjustment_new(0.0, 0.0, 24.0, 1.0, 23.0, 24.0);
  g_signal_connect(t->vadj, "value-changed", G_CALLBACK(on_vadj_changed), t);
  t->scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, t->vadj);

  gtk_box_append(GTK_BOX(hbox), t->drawing_area);
  gtk_box_append(GTK_BOX(hbox), t->scrollbar);
  gtk_box_append(GTK_BOX(t->root), hbox);

  return t;
}

void bt_terminal_close(BtTerminal *term)
{
  if (term == NULL)
    return;
  if (term->resize_timer_id != 0) {
    g_source_remove(term->resize_timer_id);
    term->resize_timer_id = 0;
  }
  if (term->pty != NULL) {
    bt_pty_close(term->pty);
    term->pty = NULL;
  }
}

void bt_terminal_free(BtTerminal *term)
{
  if (term == NULL)
    return;
  if (term->ctx_popover != NULL) {
    gtk_widget_unparent(term->ctx_popover);
    term->ctx_popover = NULL;
  }
  bt_terminal_close(term);
  if (term->search_hits != NULL)
    g_array_unref(term->search_hits);
  g_clear_object(&term->im_ctx);
  pango_font_description_free(term->font_desc);
  pango_font_description_free(term->font_bold);
  pango_font_description_free(term->font_italic);
  pango_font_description_free(term->font_bold_italic);
  bt_vterm_free(term->vt);
  g_free(term->cwd);
  g_free(term);
}

GtkWidget *bt_terminal_widget(BtTerminal *term)
{
  return term->root;
}

void bt_terminal_set_exit_cb(BtTerminal *term, BtTerminalExitFn on_exit, gpointer user_data)
{
  g_return_if_fail(term != NULL);
  term->on_exit_cb = on_exit;
  term->exit_ud = user_data;
}

gboolean bt_terminal_spawn_with_env(BtTerminal *term, const gchar *cwd, const gchar *const *envp,
                                    GError **error)
{
  g_return_val_if_fail(term != NULL && cwd != NULL, FALSE);
  bt_terminal_close(term);
  gtk_widget_set_visible(term->info_bar, FALSE);

  g_free(term->cwd);
  term->cwd = g_strdup(cwd);

  gint rows = bt_vterm_rows(term->vt);
  gint cols = bt_vterm_cols(term->vt);
  bt_vterm_free(term->vt);
  term->vt = bt_vterm_new(rows, cols, BT_SCROLLBACK_DEFAULT, on_vterm_output, term);
  term->scroll_offset = 0;
  term->has_selection = FALSE;

  BtPtySpawnOpts opts = {
    .cwd = cwd,
    .argv = NULL,
    .envp = envp,
    .rows = rows,
    .cols = cols,
    .on_data = on_pty_data,
    .on_exit = on_pty_exit,
    .on_backpressure = on_pty_backpressure,
    .user_data = term,
  };
  term->pty = bt_pty_spawn(&opts, error);
  if (term->pty == NULL) {
    show_banner(term, error && *error ? (*error)->message : "Failed to start terminal");
    return FALSE;
  }
  sync_scrollbar(term);
  gtk_widget_queue_draw(term->drawing_area);
  return TRUE;
}

gboolean bt_terminal_spawn(BtTerminal *term, const gchar *cwd, GError **error)
{
  return bt_terminal_spawn_with_env(term, cwd, NULL, error);
}

gboolean bt_terminal_write(BtTerminal *term, const guint8 *data, gsize len)
{
  if (term == NULL || term->pty == NULL)
    return FALSE;
  return bt_pty_write(term->pty, data, len);
}

void bt_terminal_focus(BtTerminal *term)
{
  if (term != NULL)
    gtk_widget_grab_focus(term->drawing_area);
}

gboolean bt_terminal_is_running(const BtTerminal *term)
{
  return term != NULL && bt_pty_is_alive(term->pty);
}

const gchar *bt_terminal_cwd(const BtTerminal *term)
{
  return term ? term->cwd : NULL;
}

guint64 bt_terminal_child_pid(const BtTerminal *term)
{
  return (term != NULL && term->pty != NULL) ? bt_pty_child_pid(term->pty) : 0;
}
