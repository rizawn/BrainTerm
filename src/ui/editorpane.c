/* editorpane.c — GtkSourceView 5 editor with centered ~72ch writing column,
 * incremental per-paragraph live-preview Markdown GtkTextTags, visual-only
 * blockquote / ## AI draft accents, 1s debounced autosave, and Ctrl+Click wiki-links. */
#include "editorpane.h"
#include "frontmatter.h"
#include "math/mathtok.h"
#include "noteutil.h"
#include "theme.h"
#include "ui/themeload.h"

#include <gtksourceview/gtksource.h>
#include <pango/pangocairo.h>
#include <string.h>

#define BT_AUTOSAVE_MS     1000u
#define BT_COLUMN_MAX_PX   760
#define BT_COLUMN_MIN_PAD  16

struct BtEditorPane {
  GtkWidget *box;
  GtkWidget *conflict_bar;
  GtkWidget *conflict_label;
  GtkWidget *placeholder;
  GtkWidget *overlay;
  GtkWidget *scrolled;
  GtkWidget *decor_area;
  GtkSourceView *view;
  GtkSourceBuffer *buffer;
  gchar *relpath;
  gboolean dirty;
  gboolean loading;
  guint autosave_id;
  gint cur_margin;

  /* Live-preview Markdown GtkTextTags (owned by buffer's tag table). */
  GtkTextTag *t_h1;
  GtkTextTag *t_h2;
  GtkTextTag *t_h3;
  GtkTextTag *t_syntax;
  GtkTextTag *t_bold;
  GtkTextTag *t_italic;
  GtkTextTag *t_code;
  GtkTextTag *t_codeblock;
  GtkTextTag *t_list_marker;
  GtkTextTag *t_quote;
  GtkTextTag *t_wikilink;
  GtkTextTag *t_tag;
  GtkTextTag *t_frontmatter;
  GtkTextTag *t_ai_heading;
  GtkTextTag *t_ai_body;
  GtkTextTag *t_math;

  BtEditorSaveFn on_save;
  BtEditorLinkFn on_link;
  BtEditorDirtyFn on_dirty;
  BtEditorDiffFn on_diff;
  BtEditorReloadFn on_reload;
  gpointer user_data;
};

/* ----------------------------------------------------- autosave & dirty -- */

static void cancel_autosave(BtEditorPane *pane)
{
  if (pane->autosave_id != 0) {
    g_source_remove(pane->autosave_id);
    pane->autosave_id = 0;
  }
}

static void emit_save(BtEditorPane *pane)
{
  if (pane->relpath == NULL || pane->on_save == NULL)
    return;
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &start, &end);
  gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &start, &end, TRUE);
  pane->on_save(pane->relpath, text, strlen(text), pane->user_data);
  g_free(text);
}

static gboolean on_autosave_timeout(gpointer ud)
{
  BtEditorPane *pane = ud;
  pane->autosave_id = 0;
  if (pane->dirty && pane->relpath != NULL)
    emit_save(pane);
  return G_SOURCE_REMOVE;
}

static void on_buffer_changed(GtkTextBuffer *buf, gpointer ud)
{
  (void)buf;
  BtEditorPane *pane = ud;
  if (pane->decor_area != NULL)
    gtk_widget_queue_draw(pane->decor_area);
  if (pane->loading || pane->relpath == NULL)
    return;
  if (!pane->dirty) {
    pane->dirty = TRUE;
    if (pane->on_dirty != NULL)
      pane->on_dirty(TRUE, pane->user_data);
  }
  cancel_autosave(pane);
  pane->autosave_id = g_timeout_add(BT_AUTOSAVE_MS, on_autosave_timeout, pane);
}

/* --------------------------------- live-preview Markdown GtkTextTags -- */

static const gchar *pick_mono_family(void)
{
  if (bt_fonts_family_available("JetBrains Mono"))
    return "JetBrains Mono";
  if (bt_fonts_family_available("Cascadia Mono"))
    return "Cascadia Mono";
  if (bt_fonts_family_available("Consolas"))
    return "Consolas";
  return "Monospace";
}

static void init_markdown_tags(BtEditorPane *p)
{
  GtkTextBuffer *b = GTK_TEXT_BUFFER(p->buffer);
  const gchar *mono = pick_mono_family();
  p->t_h1 = gtk_text_buffer_create_tag(b, "bt-h1",
      "scale", 1.65, "weight", PANGO_WEIGHT_BOLD, "foreground", "#E6E8EC",
      "pixels-above-lines", 8, "pixels-below-lines", 3, NULL);
  p->t_h2 = gtk_text_buffer_create_tag(b, "bt-h2",
      "scale", 1.35, "weight", PANGO_WEIGHT_BOLD, "foreground", "#E6E8EC",
      "pixels-above-lines", 6, "pixels-below-lines", 2, NULL);
  p->t_h3 = gtk_text_buffer_create_tag(b, "bt-h3",
      "scale", 1.15, "weight", PANGO_WEIGHT_SEMIBOLD, "foreground", "#E6E8EC",
      "pixels-above-lines", 4, "pixels-below-lines", 2, NULL);
  p->t_syntax = gtk_text_buffer_create_tag(b, "bt-syntax",
      "foreground", "#666D79", NULL);
  p->t_bold = gtk_text_buffer_create_tag(b, "bt-bold",
      "weight", PANGO_WEIGHT_BOLD, "foreground", "#E6E8EC", NULL);
  p->t_italic = gtk_text_buffer_create_tag(b, "bt-italic",
      "style", PANGO_STYLE_ITALIC, NULL);
  p->t_code = gtk_text_buffer_create_tag(b, "bt-code",
      "family", mono,
      "scale", 0.88, "background", "#1B1E24", "foreground", "#E6E8EC", NULL);
  p->t_codeblock = gtk_text_buffer_create_tag(b, "bt-codeblock",
      "family", mono,
      "scale", 0.86, "paragraph-background", "#1B1E24", "foreground", "#E6E8EC", NULL);
  p->t_list_marker = gtk_text_buffer_create_tag(b, "bt-list-marker",
      "foreground", "#8B7CF6", "weight", PANGO_WEIGHT_BOLD, NULL);
  p->t_quote = gtk_text_buffer_create_tag(b, "bt-quote",
      "foreground", "#9AA1AD", "style", PANGO_STYLE_ITALIC,
      "indent", 14, NULL);
  p->t_wikilink = gtk_text_buffer_create_tag(b, "bt-wikilink",
      "foreground", "#8B7CF6", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  p->t_tag = gtk_text_buffer_create_tag(b, "bt-tag",
      "foreground", "#8B7CF6", NULL);
  p->t_frontmatter = gtk_text_buffer_create_tag(b, "bt-frontmatter",
      "family", mono,
      "scale", 0.85, "foreground", "#666D79", NULL);
  p->t_ai_heading = gtk_text_buffer_create_tag(b, "bt-ai-heading",
      "scale", 1.25, "weight", PANGO_WEIGHT_BOLD, "foreground", "#8B7CF6",
      "paragraph-background", "#1C1A29",
      "pixels-above-lines", 6, "pixels-below-lines", 2, NULL);
  p->t_ai_body = gtk_text_buffer_create_tag(b, "bt-ai-body",
      "foreground", "#C4BFFA", "paragraph-background", "#181724", NULL);
  p->t_math = gtk_text_buffer_create_tag(b, "bt-math",
      "family", mono, "foreground", "#E6E8EC", "style", PANGO_STYLE_NORMAL, NULL);
}

static void clear_md_tags(BtEditorPane *p, const GtkTextIter *a, const GtkTextIter *b)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  GtkTextTag *all[] = {
    p->t_h1, p->t_h2, p->t_h3, p->t_syntax, p->t_bold, p->t_italic,
    p->t_code, p->t_codeblock, p->t_list_marker, p->t_quote,
    p->t_wikilink, p->t_tag, p->t_frontmatter, p->t_ai_heading, p->t_ai_body,
    p->t_math,
  };
  for (gsize i = 0; i < G_N_ELEMENTS(all); i++)
    gtk_text_buffer_remove_tag(buf, all[i], a, b);
}

static void apply_byte_tag(GtkTextBuffer *buf, GtkTextTag *tag, const GtkTextIter *line_start,
                           gint b0, gint b1)
{
  if (b1 <= b0)
    return;
  GtkTextIter s = *line_start, e = *line_start;
  gtk_text_iter_set_line_index(&s, b0);
  gtk_text_iter_set_line_index(&e, b1);
  gtk_text_buffer_apply_tag(buf, tag, &s, &e);
}

static gboolean is_ai_draft_heading_line(const gchar *line)
{
  if (!g_str_has_prefix(line, "##"))
    return FALSE;
  const gchar *p = line + 2;
  if (*p != ' ' && *p != '\t')
    return FALSE;
  while (*p == ' ' || *p == '\t')
    p++;
  return g_ascii_strncasecmp(p, "AI draft", 8) == 0 &&
         (p[8] == '\0' || p[8] == ' ' || p[8] == '\t' || p[8] == ':' || p[8] == '-');
}

static gboolean is_any_h1_or_h2(const gchar *line)
{
  return (line[0] == '#' && (line[1] == ' ' || (line[1] == '#' && line[2] == ' ')));
}

static const BtMathSpan *find_math_span_at(const GArray *mspans, gsize off)
{
  if (mspans == NULL)
    return NULL;
  for (guint k = 0; k < mspans->len; k++) {
    const BtMathSpan *sp = &g_array_index(mspans, BtMathSpan, k);
    if (off >= sp->start && off < sp->end)
      return sp;
    if (sp->start > off)
      break;
  }
  return NULL;
}

static void tag_inline_spans(BtEditorPane *p, const GtkTextIter *ls, const gchar *s)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  gint len = (gint)strlen(s);
  BtMathScanOpts mopts = bt_math_scan_default_opts();
  mopts.skip_frontmatter = FALSE;
  GArray *mspans = bt_math_scan(s, (gsize)len, &mopts);
  gint i = 0;

  while (i < len) {
    /* Inline code: `code` (checked first, §B.2.5) */
    if (s[i] == '`') {
      const gchar *close = strchr(s + i + 1, '`');
      if (close != NULL && close > s + i + 1) {
        gint end = (gint)(close - s);
        apply_byte_tag(buf, p->t_syntax, ls, i, i + 1);
        apply_byte_tag(buf, p->t_code, ls, i + 1, end);
        apply_byte_tag(buf, p->t_syntax, ls, end, end + 1);
        i = end + 1;
        continue;
      }
    }
    /* Math spans ($...$, $$...$$, \(...\), \[...\]): exclude all emphasis & link tags (§B.2.5). */
    const BtMathSpan *ms = find_math_span_at(mspans, (gsize)i);
    if (ms != NULL && (gint)ms->start == i) {
      apply_byte_tag(buf, p->t_syntax, ls, (gint)ms->start, (gint)ms->src_start);
      apply_byte_tag(buf, p->t_math, ls, (gint)ms->src_start, (gint)ms->src_end);
      apply_byte_tag(buf, p->t_syntax, ls, (gint)ms->src_end, (gint)ms->end);
      i = (gint)ms->end;
      continue;
    }
    /* Wiki-link: [[target]] */
    if (s[i] == '[' && i + 1 < len && s[i + 1] == '[') {
      const gchar *close = strstr(s + i + 2, "]]");
      if (close != NULL && close > s + i + 2 &&
          find_math_span_at(mspans, (gsize)(close - s)) == NULL) {
        gint end = (gint)(close - s);
        apply_byte_tag(buf, p->t_syntax, ls, i, i + 2);
        apply_byte_tag(buf, p->t_wikilink, ls, i + 2, end);
        apply_byte_tag(buf, p->t_syntax, ls, end, end + 2);
        i = end + 2;
        continue;
      }
    }
    /* Bold: **text** */
    if (s[i] == '*' && i + 1 < len && s[i + 1] == '*') {
      const gchar *close = strstr(s + i + 2, "**");
      if (close != NULL && close > s + i + 2 &&
          find_math_span_at(mspans, (gsize)(close - s)) == NULL) {
        gint end = (gint)(close - s);
        apply_byte_tag(buf, p->t_syntax, ls, i, i + 2);
        apply_byte_tag(buf, p->t_bold, ls, i + 2, end);
        apply_byte_tag(buf, p->t_syntax, ls, end, end + 2);
        i = end + 2;
        continue;
      }
    }
    /* Italic: *text* (not **; never cross into a math span) */
    if (s[i] == '*' && (i + 1 >= len || s[i + 1] != '*')) {
      const gchar *close = strchr(s + i + 1, '*');
      if (close != NULL && close > s + i + 1 && s[i + 1] != ' ' &&
          find_math_span_at(mspans, (gsize)(close - s)) == NULL) {
        gint end = (gint)(close - s);
        apply_byte_tag(buf, p->t_syntax, ls, i, i + 1);
        apply_byte_tag(buf, p->t_italic, ls, i + 1, end);
        apply_byte_tag(buf, p->t_syntax, ls, end, end + 1);
        i = end + 1;
        continue;
      }
    }
    /* Inline #tag */
    if (s[i] == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') &&
        i + 1 < len && g_ascii_isalpha(s[i + 1])) {
      gint j = i + 2;
      while (j < len && (g_ascii_isalnum(s[j]) || s[j] == '_' || s[j] == '-' || s[j] == '/'))
        j++;
      apply_byte_tag(buf, p->t_tag, ls, i, j);
      i = j;
      continue;
    }
    i++;
  }
  g_array_unref(mspans);
}

typedef struct {
  gboolean in_frontmatter;
  gboolean in_codeblock;
  gboolean in_mathblock;
  gboolean in_ai_draft;
} LineCtx;

static guint count_unescaped_dd_outside_code(const gchar *line, gsize *first_pos, gsize *last_pos)
{
  guint count = 0;
  gboolean in_code = FALSE;
  gsize n = strlen(line);
  if (first_pos) *first_pos = G_MAXSIZE;
  if (last_pos) *last_pos = G_MAXSIZE;
  for (gsize i = 0; i < n; i++) {
    if (line[i] == '`') {
      in_code = !in_code;
      continue;
    }
    if (!in_code && line[i] == '$' && i + 1 < n && line[i + 1] == '$') {
      gsize bs = 0;
      while (i > bs && line[i - 1 - bs] == '\\')
        bs++;
      if ((bs & 1u) == 0u) {
        if (count == 0 && first_pos)
          *first_pos = i;
        if (last_pos)
          *last_pos = i;
        count++;
        i++;
      }
    }
  }
  return count;
}

static void scan_line_context(GtkTextBuffer *buf, gint target_line, LineCtx *ctx)
{
  memset(ctx, 0, sizeof(*ctx));
  gint n_lines = gtk_text_buffer_get_line_count(buf);
  gint limit = MIN(target_line, n_lines);

  for (gint ln = 0; ln < limit; ln++) {
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line(buf, &s, ln);
    e = s;
    if (!gtk_text_iter_ends_line(&e))
      gtk_text_iter_forward_to_line_end(&e);
    g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    const gchar *trim = txt;
    while (*trim == ' ' || *trim == '\t')
      trim++;

    if (ln == 0 && strcmp(trim, "---") == 0) {
      ctx->in_frontmatter = TRUE;
      continue;
    }
    if (ctx->in_frontmatter) {
      if (strcmp(trim, "---") == 0)
        ctx->in_frontmatter = FALSE;
      continue;
    }
    if (!ctx->in_mathblock && g_str_has_prefix(trim, "```")) {
      ctx->in_codeblock = !ctx->in_codeblock;
      continue;
    }
    if (!ctx->in_codeblock) {
      if ((count_unescaped_dd_outside_code(txt, NULL, NULL) & 1u) != 0u)
        ctx->in_mathblock = !ctx->in_mathblock;
      if (!ctx->in_mathblock) {
        if (is_ai_draft_heading_line(txt))
          ctx->in_ai_draft = TRUE;
        else if (ctx->in_ai_draft && is_any_h1_or_h2(txt))
          ctx->in_ai_draft = FALSE;
      }
    }
  }
}

static void tag_single_line(BtEditorPane *p, gint ln, LineCtx *ctx)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  GtkTextIter s, e;
  gtk_text_buffer_get_iter_at_line(buf, &s, ln);
  e = s;
  if (!gtk_text_iter_ends_line(&e))
    gtk_text_iter_forward_to_line_end(&e);

  GtkTextIter clear_end = e;
  gtk_text_iter_forward_line(&clear_end);
  clear_md_tags(p, &s, &clear_end);

  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
  const gchar *trim = txt;
  while (*trim == ' ' || *trim == '\t')
    trim++;

  /* Frontmatter block */
  if (ln == 0 && strcmp(trim, "---") == 0) {
    ctx->in_frontmatter = TRUE;
    gtk_text_buffer_apply_tag(buf, p->t_frontmatter, &s, &e);
    return;
  }
  if (ctx->in_frontmatter) {
    gtk_text_buffer_apply_tag(buf, p->t_frontmatter, &s, &e);
    if (strcmp(trim, "---") == 0)
      ctx->in_frontmatter = FALSE;
    return;
  }

  /* Fenced code blocks (never toggle inside multi-line math) */
  if (!ctx->in_mathblock && g_str_has_prefix(trim, "```")) {
    ctx->in_codeblock = !ctx->in_codeblock;
    gtk_text_buffer_apply_tag(buf, p->t_codeblock, &s, &e);
    gtk_text_buffer_apply_tag(buf, p->t_syntax, &s, &e);
    return;
  }
  if (ctx->in_codeblock) {
    gtk_text_buffer_apply_tag(buf, p->t_codeblock, &s, &e);
    return;
  }

  /* Multi-line $$ display math blocks: never apply emphasis or links inside */
  gsize first_dd = G_MAXSIZE, last_dd = G_MAXSIZE;
  guint dd_count = count_unescaped_dd_outside_code(txt, &first_dd, &last_dd);
  if (ctx->in_mathblock) {
    gtk_text_buffer_apply_tag(buf, p->t_math, &s, &e);
    if ((dd_count & 1u) != 0u) {
      if (last_dd != G_MAXSIZE)
        apply_byte_tag(buf, p->t_syntax, &s, (gint)last_dd, (gint)(last_dd + 2));
      ctx->in_mathblock = FALSE;
    }
    return;
  }
  if ((dd_count & 1u) != 0u) {
    ctx->in_mathblock = TRUE;
    gtk_text_buffer_apply_tag(buf, p->t_math, &s, &e);
    if (first_dd != G_MAXSIZE)
      apply_byte_tag(buf, p->t_syntax, &s, (gint)first_dd, (gint)(first_dd + 2));
    return;
  }

  /* ## AI draft section detection (visual only; file untouched) */
  if (is_ai_draft_heading_line(txt)) {
    ctx->in_ai_draft = TRUE;
    gtk_text_buffer_apply_tag(buf, p->t_ai_heading, &s, &e);
    apply_byte_tag(buf, p->t_syntax, &s, 0, 3);
    return;
  }
  if (ctx->in_ai_draft && is_any_h1_or_h2(txt))
    ctx->in_ai_draft = FALSE;

  if (ctx->in_ai_draft) {
    gtk_text_buffer_apply_tag(buf, p->t_ai_body, &s, &e);
    tag_inline_spans(p, &s, txt);
    return;
  }

  /* Headings # / ## / ### */
  if (txt[0] == '#') {
    gint level = 0;
    while (txt[level] == '#' && level < 4)
      level++;
    if (level >= 1 && level <= 3 && txt[level] == ' ') {
      GtkTextTag *htag = (level == 1) ? p->t_h1 : (level == 2 ? p->t_h2 : p->t_h3);
      gtk_text_buffer_apply_tag(buf, htag, &s, &e);
      apply_byte_tag(buf, p->t_syntax, &s, 0, level + 1);
      tag_inline_spans(p, &s, txt);
      return;
    }
  }

  /* Blockquote > */
  if (trim[0] == '>') {
    gint off = (gint)(trim - txt);
    gtk_text_buffer_apply_tag(buf, p->t_quote, &s, &e);
    apply_byte_tag(buf, p->t_syntax, &s, off, off + 1);
    tag_inline_spans(p, &s, txt);
    return;
  }

  /* List markers: -, *, +, or 1. */
  gint lead = (gint)(trim - txt);
  if ((trim[0] == '-' || trim[0] == '*' || trim[0] == '+') && trim[1] == ' ') {
    apply_byte_tag(buf, p->t_list_marker, &s, lead, lead + 1);
  } else if (g_ascii_isdigit(trim[0])) {
    gint d = 0;
    while (g_ascii_isdigit(trim[d]))
      d++;
    if (trim[d] == '.' && trim[d + 1] == ' ')
      apply_byte_tag(buf, p->t_list_marker, &s, lead, lead + d + 1);
  }

  tag_inline_spans(p, &s, txt);
}

static void retag_lines(BtEditorPane *p, gint start_line, gint end_line)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  gint total = gtk_text_buffer_get_line_count(buf);
  if (total <= 0)
    return;
  start_line = CLAMP(start_line, 0, total - 1);
  end_line = CLAMP(end_line, start_line, total - 1);

  LineCtx ctx;
  scan_line_context(buf, start_line, &ctx);
  for (gint ln = start_line; ln <= end_line; ln++)
    tag_single_line(p, ln, &ctx);
}

static void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *loc, gchar *text, gint len,
                                 gpointer ud)
{
  BtEditorPane *p = ud;
  if (p->loading)
    return;
  gint end_ln = gtk_text_iter_get_line(loc);
  gint newlines = 0;
  for (gint i = 0; i < len; i++)
    if (text[i] == '\n')
      newlines++;
  gint start_ln = MAX(end_ln - newlines, 0);

  /* Fence/heading/frontmatter/display-math edits can affect subsequent lines. */
  gboolean structural = (start_ln == 0) || strstr(text, "```") != NULL ||
                        strstr(text, "---") != NULL || strstr(text, "##") != NULL ||
                        strstr(text, "$$") != NULL;
  if (structural)
    retag_lines(p, start_ln, gtk_text_buffer_get_line_count(buf) - 1);
  else
    retag_lines(p, start_ln, end_ln);
}

static void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end,
                                  gpointer ud)
{
  (void)end;
  BtEditorPane *p = ud;
  if (p->loading)
    return;
  gint ln = gtk_text_iter_get_line(start);
  gint total = gtk_text_buffer_get_line_count(buf);
  retag_lines(p, ln, MIN(ln + 1, total - 1));
}

/* --------------------- centered ~72ch column & visual gutter overlay -- */

static void update_column_margins(BtEditorPane *p)
{
  if (p->scrolled == NULL || p->view == NULL)
    return;
  gint m = BT_COLUMN_MIN_PAD;
  if (!gtk_source_view_get_show_line_numbers(p->view)) {
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(p->scrolled));
    gint vw = hadj ? (gint)gtk_adjustment_get_page_size(hadj) : gtk_widget_get_width(p->scrolled);
    if (vw <= 0)
      vw = gtk_widget_get_width(p->scrolled);
    if (vw > BT_COLUMN_MAX_PX + BT_COLUMN_MIN_PAD * 2)
      m = (vw - BT_COLUMN_MAX_PX) / 2;
  }
  if (m != p->cur_margin) {
    p->cur_margin = m;
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(p->view), m);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(p->view), m);
    if (p->decor_area != NULL)
      gtk_widget_queue_draw(p->decor_area);
  }
}

static void on_scrolled_page_size(GObject *obj, GParamSpec *pspec, gpointer ud)
{
  (void)obj; (void)pspec;
  update_column_margins(ud);
}

static void on_vscroll_changed(GtkAdjustment *adj, gpointer ud)
{
  (void)adj;
  BtEditorPane *p = ud;
  if (p->decor_area != NULL)
    gtk_widget_queue_draw(p->decor_area);
}

static void draw_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r)
{
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0.0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0.0, G_PI / 2.0);
  cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
  cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
  cairo_close_path(cr);
}

/* Draws subtle left bars for blockquotes and soft-violet bar + "AI draft" pill
 * for ## AI draft sections across visible lines only (visual overlay, zero edits). */
static void on_decor_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
  (void)area; (void)h;
  BtEditorPane *p = ud;
  if (p->view == NULL || p->buffer == NULL || p->relpath == NULL)
    return;

  GdkRectangle vis;
  gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(p->view), &vis);
  if (vis.height <= 0)
    return;

  GtkTextIter top_it, bot_it;
  gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(p->view), &top_it, vis.y, NULL);
  gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(p->view), &bot_it, vis.y + vis.height, NULL);
  gint l0 = gtk_text_iter_get_line(&top_it);
  gint l1 = gtk_text_iter_get_line(&bot_it);

  /* Compute the X offset of the text column inside the widget. */
  gint col_x = 0, tmp_y = 0;
  gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(p->view), GTK_TEXT_WINDOW_WIDGET,
                                        0, vis.y, &col_x, &tmp_y);
  gdouble bar_x = (gdouble)(col_x + p->cur_margin - 12);
  if (bar_x < 6.0)
    bar_x = 6.0;

  gdouble ar, ag, ab;
  bt_theme_unpack(BT_COL_ACCENT, &ar, &ag, &ab);
  gdouble br, bg, bb;
  bt_theme_unpack(BT_COL_BORDER_STRONG, &br, &bg, &bb);

  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  for (gint ln = l0; ln <= l1; ln++) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(buf, &it, ln);
    gboolean is_ai_h = gtk_text_iter_has_tag(&it, p->t_ai_heading);
    gboolean is_ai_b = gtk_text_iter_has_tag(&it, p->t_ai_body);
    gboolean is_qt = gtk_text_iter_has_tag(&it, p->t_quote);
    if (!is_ai_h && !is_ai_b && !is_qt)
      continue;

    gint by = 0, bh = 0;
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(p->view), &it, &by, &bh);
    gint wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(p->view), GTK_TEXT_WINDOW_WIDGET,
                                          0, by, &wx, &wy);
    if (is_ai_h || is_ai_b) {
      cairo_set_source_rgba(cr, ar, ag, ab, is_ai_h ? 0.90 : 0.65);
      cairo_rectangle(cr, bar_x, (gdouble)wy, 3.0, (gdouble)bh);
      cairo_fill(cr);

      if (is_ai_h) {
        PangoLayout *pl = pango_cairo_create_layout(cr);
        PangoFontDescription *fd = pango_font_description_from_string("Inter, Segoe UI, Sans Semi-Bold 9");
        pango_layout_set_font_description(pl, fd);
        pango_layout_set_text(pl, "AI draft", -1);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(pl, &tw, &th);
        gdouble pw = (gdouble)tw + 14.0;
        gdouble ph = (gdouble)th + 6.0;
        gdouble px = MIN((gdouble)(w - p->cur_margin) - pw, (gdouble)w - pw - 16.0);
        gdouble py = (gdouble)wy + MAX(((gdouble)bh - ph) * 0.5, 2.0);
        if (px > bar_x + 120.0) {
          draw_rounded_rect(cr, px, py, pw, ph, ph * 0.5);
          cairo_set_source_rgba(cr, ar, ag, ab, 0.16);
          cairo_fill_preserve(cr);
          cairo_set_source_rgba(cr, ar, ag, ab, 0.45);
          cairo_set_line_width(cr, 1.0);
          cairo_stroke(cr);
          cairo_set_source_rgba(cr, ar, ag, ab, 1.0);
          cairo_move_to(cr, px + 7.0, py + 3.0);
          pango_cairo_show_layout(cr, pl);
        }
        pango_font_description_free(fd);
        g_object_unref(pl);
      }
    } else if (is_qt) {
      cairo_set_source_rgba(cr, br, bg, bb, 0.95);
      cairo_rectangle(cr, bar_x + 2.0, (gdouble)wy + 2.0, 2.0, MAX((gdouble)bh - 4.0, 4.0));
      cairo_fill(cr);
    }
  }
}

/* ----------------------------------------------------- wiki-link click -- */

static void on_editor_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                            gpointer ud)
{
  BtEditorPane *pane = ud;
  if (n_press != 1 || pane->on_link == NULL || pane->relpath == NULL)
    return;
  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
  GdkEvent *ev = gtk_gesture_get_last_event(GTK_GESTURE(gesture), seq);
  if (ev == NULL)
    return;
  GdkModifierType state = gdk_event_get_modifier_state(ev);
  if ((state & GDK_CONTROL_MASK) == 0)
    return;

  gint bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(pane->view), GTK_TEXT_WINDOW_WIDGET,
                                        (gint)x, (gint)y, &bx, &by);
  GtkTextIter iter;
  if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(pane->view), &iter, bx, by))
    return;

  GtkTextIter line_start = iter, line_end = iter;
  gtk_text_iter_set_line_offset(&line_start, 0);
  if (!gtk_text_iter_ends_line(&line_end))
    gtk_text_iter_forward_to_line_end(&line_end);

  gchar *line = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &line_start, &line_end,
                                         FALSE);
  gint line_byte_off = gtk_text_iter_get_line_index(&iter);
  gchar *target = bt_wikilink_at(line, (gsize)MAX(line_byte_off, 0));
  g_free(line);
  if (target != NULL) {
    pane->on_link(target, pane->user_data);
    g_free(target);
  }
}

/* ------------------------------------------- language & scheme loading -- */

static gchar *find_exe_dir(void)
{
#ifdef G_OS_WIN32
  return g_win32_get_package_installation_directory_of_module(NULL);
#else
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe == NULL)
    return NULL;
  gchar *dir = g_path_get_dirname(exe);
  g_free(exe);
  return dir;
#endif
}

static gboolean path_array_has(const gchar *const *arr, const gchar *path)
{
  for (guint j = 0; arr != NULL && arr[j] != NULL; j++) {
    if (strcmp(arr[j], path) == 0)
      return TRUE;
  }
  return FALSE;
}

static void add_dir_candidate(GPtrArray *out, const gchar *path, gboolean require_dir)
{
  if (path == NULL || path[0] == '\0')
    return;
  if (require_dir && !g_file_test(path, G_FILE_TEST_IS_DIR))
    return;
  g_ptr_array_add(out, g_strdup(path));
}

static GPtrArray *collect_subdir_paths(const gchar *compile_dir, const gchar *subdir,
                                       const gchar *res_uri)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (res_uri != NULL)
    add_dir_candidate(out, res_uri, FALSE);
  add_dir_candidate(out, compile_dir, TRUE);

  gchar *cwd = g_get_current_dir();
  gchar *cwd_dir = g_build_filename(cwd, "data", subdir, NULL);
  add_dir_candidate(out, cwd_dir, TRUE);
  g_free(cwd_dir);
  g_free(cwd);

  gchar *exe_dir = find_exe_dir();
  if (exe_dir != NULL) {
    gchar *pkg1 = g_build_filename(exe_dir, "share", "brainterm", subdir, NULL);
    gchar *pkg2 = g_build_filename(exe_dir, "..", "share", "brainterm", subdir, NULL);
    gchar *dev1 = g_build_filename(exe_dir, "..", "..", "data", subdir, NULL);
    add_dir_candidate(out, pkg1, TRUE);
    add_dir_candidate(out, pkg2, TRUE);
    add_dir_candidate(out, dev1, TRUE);
    g_free(pkg1);
    g_free(pkg2);
    g_free(dev1);
    g_free(exe_dir);
  }

  const gchar *const *sys_dirs = g_get_system_data_dirs();
  for (guint i = 0; sys_dirs != NULL && sys_dirs[i] != NULL; i++) {
    gchar *p = g_build_filename(sys_dirs[i], "brainterm", subdir, NULL);
    add_dir_candidate(out, p, TRUE);
    g_free(p);
  }
  return out;
}

static void configure_language(GtkSourceBuffer *buffer)
{
  GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
  const gchar *compile_dir = NULL;
#ifdef BT_LANGSPEC_DIR
  compile_dir = BT_LANGSPEC_DIR;
#endif
  GPtrArray *paths = collect_subdir_paths(compile_dir, "language-specs", NULL);
  const gchar *const *cur = gtk_source_language_manager_get_search_path(lm);
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = g_ptr_array_index(paths, i);
    if (!path_array_has(cur, p)) {
      gtk_source_language_manager_append_search_path(lm, p);
      cur = gtk_source_language_manager_get_search_path(lm);
    }
  }
  g_ptr_array_unref(paths);

  GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, "bt-markdown");
  if (lang == NULL)
    lang = gtk_source_language_manager_get_language(lm, "markdown");
  if (lang != NULL) {
    gtk_source_buffer_set_language(buffer, lang);
    gtk_source_buffer_set_highlight_syntax(buffer, TRUE);
  }
}

static void apply_ink_scheme(GtkSourceBuffer *buffer, gboolean dark)
{
  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  const gchar *compile_dir = NULL;
#ifdef BT_STYLES_DIR
  compile_dir = BT_STYLES_DIR;
#endif
  GPtrArray *paths = collect_subdir_paths(compile_dir, "styles", "resource:///local/brainterm/styles");
  const gchar *const *cur = gtk_source_style_scheme_manager_get_search_path(sm);
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = g_ptr_array_index(paths, i);
    if (!path_array_has(cur, p)) {
      gtk_source_style_scheme_manager_append_search_path(sm, p);
      cur = gtk_source_style_scheme_manager_get_search_path(sm);
    }
  }
  g_ptr_array_unref(paths);

  GtkSourceStyleScheme *sc =
      gtk_source_style_scheme_manager_get_scheme(sm, dark ? "bt-ink-dark" : "bt-ink-light");
  if (sc != NULL)
    gtk_source_buffer_set_style_scheme(buffer, sc);
}

/* ----------------------------------------------------- conflict banner -- */

static void on_conflict_diff(GtkButton *b, gpointer ud)
{
  (void)b;
  BtEditorPane *p = ud;
  if (p->on_diff != NULL && p->relpath != NULL)
    p->on_diff(p->relpath, p->user_data);
}

static void on_conflict_keep(GtkButton *b, gpointer ud)
{
  (void)b;
  BtEditorPane *p = ud;
  gtk_widget_set_visible(p->conflict_bar, FALSE);
  bt_editorpane_flush_save(p);
}

static void on_conflict_reload(GtkButton *b, gpointer ud)
{
  (void)b;
  BtEditorPane *p = ud;
  gtk_widget_set_visible(p->conflict_bar, FALSE);
  if (p->on_reload != NULL && p->relpath != NULL)
    p->on_reload(p->relpath, p->user_data);
}

/* ---------------------------------------------------------- public API -- */

BtEditorPane *bt_editorpane_new(BtEditorSaveFn on_save, BtEditorLinkFn on_link,
                                BtEditorDirtyFn on_dirty, gpointer user_data)
{
  BtEditorPane *pane = g_new0(BtEditorPane, 1);
  pane->on_save = on_save;
  pane->on_link = on_link;
  pane->on_dirty = on_dirty;
  pane->user_data = user_data;
  pane->cur_margin = BT_COLUMN_MIN_PAD;

  pane->buffer = gtk_source_buffer_new(NULL);
  configure_language(pane->buffer);
  apply_ink_scheme(pane->buffer, TRUE);
  init_markdown_tags(pane);
  g_signal_connect(pane->buffer, "changed", G_CALLBACK(on_buffer_changed), pane);
  g_signal_connect_after(pane->buffer, "insert-text", G_CALLBACK(on_insert_text_after), pane);
  g_signal_connect_after(pane->buffer, "delete-range", G_CALLBACK(on_delete_range_after), pane);

  pane->view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(pane->buffer));
  gtk_widget_set_name(GTK_WIDGET(pane->view), "bt-editor-view");
  gtk_widget_add_css_class(GTK_WIDGET(pane->view), "bt-editor");
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(pane->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(pane->view), BT_COLUMN_MIN_PAD);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(pane->view), BT_COLUMN_MIN_PAD);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(pane->view), 14);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(pane->view), 16);
  gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(pane->view), 1);
  gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(pane->view), 1);
  gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(pane->view), 2);
  gtk_source_view_set_show_line_numbers(pane->view, TRUE);
  gtk_source_view_set_highlight_current_line(pane->view, TRUE);
  gtk_source_view_set_tab_width(pane->view, 2);
  gtk_source_view_set_insert_spaces_instead_of_tabs(pane->view, TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(pane->view), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(pane->view), TRUE);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_editor_click), pane);
  gtk_widget_add_controller(GTK_WIDGET(pane->view), GTK_EVENT_CONTROLLER(click));

  pane->scrolled = gtk_scrolled_window_new();
  gtk_widget_set_name(pane->scrolled, "bt-editor-scrolled");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pane->scrolled), GTK_WIDGET(pane->view));
  gtk_widget_set_vexpand(pane->scrolled, TRUE);
  gtk_widget_set_hexpand(pane->scrolled, TRUE);

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
  if (hadj != NULL)
    g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_scrolled_page_size), pane);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
  if (vadj != NULL)
    g_signal_connect(vadj, "value-changed", G_CALLBACK(on_vscroll_changed), pane);

  pane->decor_area = gtk_drawing_area_new();
  gtk_widget_set_can_target(pane->decor_area, FALSE);
  gtk_widget_set_focusable(pane->decor_area, FALSE);
  gtk_widget_set_hexpand(pane->decor_area, TRUE);
  gtk_widget_set_vexpand(pane->decor_area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pane->decor_area), on_decor_draw, pane, NULL);

  pane->overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(pane->overlay), pane->scrolled);
  gtk_overlay_add_overlay(GTK_OVERLAY(pane->overlay), pane->decor_area);
  gtk_widget_set_hexpand(pane->overlay, TRUE);
  gtk_widget_set_vexpand(pane->overlay, TRUE);

  /* Empty state placeholder */
  pane->placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(pane->placeholder, "bt-editor-placeholder");
  gtk_widget_add_css_class(pane->placeholder, "bt-empty");
  gtk_widget_set_halign(pane->placeholder, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(pane->placeholder, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(pane->placeholder, TRUE);
  gtk_widget_set_hexpand(pane->placeholder, TRUE);
  GtkWidget *pico = gtk_image_new_from_icon_name("bt-editor-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(pico), 36);
  GtkWidget *ptitle = gtk_label_new("No note selected");
  gtk_widget_add_css_class(ptitle, "bt-empty-title");
  GtkWidget *psub = gtk_label_new("Choose a Markdown note from the sidebar or press Ctrl+P");
  gtk_box_append(GTK_BOX(pane->placeholder), pico);
  gtk_box_append(GTK_BOX(pane->placeholder), ptitle);
  gtk_box_append(GTK_BOX(pane->placeholder), psub);

  /* Conflict banner (§6.A5): never auto-overwrites either side. */
  pane->conflict_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(pane->conflict_bar, "bt-banner");
  pane->conflict_label = gtk_label_new("File changed on disk (an agent may have edited it)");
  gtk_label_set_xalign(GTK_LABEL(pane->conflict_label), 0.0f);
  gtk_widget_set_hexpand(pane->conflict_label, TRUE);
  GtkWidget *b_diff = gtk_button_new_with_label("Review diff");
  GtkWidget *b_keep = gtk_button_new_with_label("Keep mine");
  GtkWidget *b_rel = gtk_button_new_with_label("Reload");
  gtk_widget_add_css_class(b_diff, "bt-hairline");
  gtk_widget_add_css_class(b_keep, "bt-hairline");
  gtk_widget_add_css_class(b_rel, "bt-hairline");
  g_signal_connect(b_diff, "clicked", G_CALLBACK(on_conflict_diff), pane);
  g_signal_connect(b_keep, "clicked", G_CALLBACK(on_conflict_keep), pane);
  g_signal_connect(b_rel, "clicked", G_CALLBACK(on_conflict_reload), pane);
  gtk_box_append(GTK_BOX(pane->conflict_bar), pane->conflict_label);
  gtk_box_append(GTK_BOX(pane->conflict_bar), b_diff);
  gtk_box_append(GTK_BOX(pane->conflict_bar), b_keep);
  gtk_box_append(GTK_BOX(pane->conflict_bar), b_rel);
  gtk_widget_set_visible(pane->conflict_bar, FALSE);

  pane->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(pane->box, "bt-editor-box");
  gtk_widget_set_hexpand(pane->box, TRUE);
  gtk_widget_set_vexpand(pane->box, TRUE);
  gtk_box_append(GTK_BOX(pane->box), pane->conflict_bar);
  gtk_box_append(GTK_BOX(pane->box), pane->placeholder);
  gtk_box_append(GTK_BOX(pane->box), pane->overlay);
  gtk_widget_set_visible(pane->overlay, FALSE);

  return pane;
}

void bt_editorpane_set_conflict_cbs(BtEditorPane *pane, BtEditorDiffFn on_diff,
                                    BtEditorReloadFn on_reload)
{
  if (pane == NULL)
    return;
  pane->on_diff = on_diff;
  pane->on_reload = on_reload;
}

void bt_editorpane_show_conflict(BtEditorPane *pane, gboolean deleted)
{
  if (pane == NULL)
    return;
  gtk_label_set_text(GTK_LABEL(pane->conflict_label),
                     deleted ? "File was deleted on disk — [Keep mine] to recreate or [Reload] to close"
                             : "File changed on disk (an agent may have edited it)");
  gtk_widget_set_visible(pane->conflict_bar, TRUE);
}

void bt_editorpane_hide_conflict(BtEditorPane *pane)
{
  if (pane != NULL)
    gtk_widget_set_visible(pane->conflict_bar, FALSE);
}

void bt_editorpane_set_dark(BtEditorPane *pane, gboolean dark)
{
  if (pane != NULL && pane->buffer != NULL)
    apply_ink_scheme(pane->buffer, dark);
}

void bt_editorpane_set_line_numbers(BtEditorPane *pane, gboolean show)
{
  if (pane != NULL && pane->view != NULL) {
    gtk_source_view_set_show_line_numbers(pane->view, show);
    update_column_margins(pane);
  }
}

gchar *bt_editorpane_buffer_hash(const BtEditorPane *pane)
{
  if (pane == NULL)
    return NULL;
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &s, &e);
  gchar *txt = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &s, &e, TRUE);
  gchar *h = bt_fnv1a_hex(txt, strlen(txt));
  g_free(txt);
  return h;
}

void bt_editorpane_free(BtEditorPane *pane)
{
  if (pane == NULL)
    return;
  cancel_autosave(pane);
  g_free(pane->relpath);
  g_clear_object(&pane->buffer);
  g_free(pane);
}

GtkWidget *bt_editorpane_widget(BtEditorPane *pane)
{
  return pane->box;
}

GtkWidget *bt_editorpane_view_widget(BtEditorPane *pane)
{
  return pane ? GTK_WIDGET(pane->view) : NULL;
}

void bt_editorpane_load(BtEditorPane *pane, const gchar *relpath, const gchar *text, gsize length)
{
  g_return_if_fail(pane != NULL && relpath != NULL);
  cancel_autosave(pane);
  g_free(pane->relpath);
  pane->relpath = g_strdup(relpath);

  pane->loading = TRUE;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pane->buffer), text ? text : "", (gint)length);
  gint nlines = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(pane->buffer));
  retag_lines(pane, 0, MAX(nlines - 1, 0));
  GtkTextIter start;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(pane->buffer), &start);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(pane->buffer), &start);
  pane->loading = FALSE;

  pane->dirty = FALSE;
  if (pane->on_dirty != NULL)
    pane->on_dirty(FALSE, pane->user_data);
  gtk_widget_set_visible(pane->placeholder, FALSE);
  gtk_widget_set_visible(pane->overlay, TRUE);
  update_column_margins(pane);
  if (pane->decor_area != NULL)
    gtk_widget_queue_draw(pane->decor_area);
  gtk_widget_grab_focus(GTK_WIDGET(pane->view));
}

void bt_editorpane_clear(BtEditorPane *pane)
{
  g_return_if_fail(pane != NULL);
  cancel_autosave(pane);
  g_clear_pointer(&pane->relpath, g_free);
  pane->loading = TRUE;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pane->buffer), "", 0);
  pane->loading = FALSE;
  pane->dirty = FALSE;
  gtk_widget_set_visible(pane->overlay, FALSE);
  gtk_widget_set_visible(pane->placeholder, TRUE);
}

const gchar *bt_editorpane_relpath(const BtEditorPane *pane)
{
  return pane ? pane->relpath : NULL;
}

gboolean bt_editorpane_is_dirty(const BtEditorPane *pane)
{
  return pane ? pane->dirty : FALSE;
}

const gchar *bt_editorpane_scheme_id(const BtEditorPane *pane)
{
  if (pane == NULL || pane->buffer == NULL)
    return "none";
  GtkSourceStyleScheme *sc = gtk_source_buffer_get_style_scheme(pane->buffer);
  return sc ? gtk_source_style_scheme_get_id(sc) : "none";
}

void bt_editorpane_flush_save(BtEditorPane *pane)
{
  if (pane == NULL || pane->relpath == NULL)
    return;
  cancel_autosave(pane);
  emit_save(pane);
}

void bt_editorpane_mark_saved(BtEditorPane *pane)
{
  if (pane == NULL)
    return;
  pane->dirty = FALSE;
  if (pane->on_dirty != NULL)
    pane->on_dirty(FALSE, pane->user_data);
}

static gboolean iter_at_utf8_byte(GtkTextBuffer *buf, gsize byte_off, GtkTextIter *out)
{
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(buf, &s, &e);
  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
  gsize total = strlen(txt);
  if (byte_off > total)
    byte_off = total;
  gint char_off = (gint)g_utf8_strlen(txt, (gssize)byte_off);
  gtk_text_buffer_get_iter_at_offset(buf, out, char_off);
  return TRUE;
}

gboolean bt_editorpane_range_has_emphasis_tag(const BtEditorPane *pane, gsize byte_start, gsize byte_end)
{
  if (pane == NULL || pane->buffer == NULL || byte_end <= byte_start)
    return FALSE;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  GtkTextIter s, e;
  iter_at_utf8_byte(buf, byte_start, &s);
  iter_at_utf8_byte(buf, byte_end, &e);
  gtk_source_buffer_ensure_highlight(pane->buffer, &s, &e);

  GtkTextIter it = s;
  while (gtk_text_iter_compare(&it, &e) < 0) {
    GSList *tags = gtk_text_iter_get_tags(&it);
    for (GSList *l = tags; l != NULL; l = l->next) {
      GtkTextTag *tag = GTK_TEXT_TAG(l->data);
      gboolean style_set = FALSE;
      PangoStyle pstyle = PANGO_STYLE_NORMAL;
      g_object_get(tag, "style-set", &style_set, "style", &pstyle, NULL);
      if (tag == pane->t_italic || (style_set && pstyle == PANGO_STYLE_ITALIC)) {
        g_slist_free(tags);
        return TRUE;
      }
    }
    g_slist_free(tags);
    if (!gtk_text_iter_forward_char(&it))
      break;
  }
  return FALSE;
}

gboolean bt_editorpane_offset_has_tag_name(const BtEditorPane *pane, gsize byte_offset, const gchar *tag_name)
{
  if (pane == NULL || pane->buffer == NULL || tag_name == NULL)
    return FALSE;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  GtkTextIter s, e, bstart, bend;
  gtk_text_buffer_get_bounds(buf, &bstart, &bend);
  gtk_source_buffer_ensure_highlight(pane->buffer, &bstart, &bend);
  iter_at_utf8_byte(buf, byte_offset, &s);
  e = s;
  gtk_text_iter_forward_char(&e);

  GSList *tags = gtk_text_iter_get_tags(&s);
  gboolean found = FALSE;
  for (GSList *l = tags; l != NULL; l = l->next) {
    g_autofree gchar *name = NULL;
    g_object_get(GTK_TEXT_TAG(l->data), "name", &name, NULL);
    if (name != NULL && strcmp(name, tag_name) == 0) {
      found = TRUE;
      break;
    }
  }
  g_slist_free(tags);
  return found;
}

const gchar *bt_editorpane_language_id(const BtEditorPane *pane)
{
  if (pane == NULL || pane->buffer == NULL)
    return "none";
  GtkSourceLanguage *lang = gtk_source_buffer_get_language(pane->buffer);
  return lang ? gtk_source_language_get_id(lang) : "none";
}
