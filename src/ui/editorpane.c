/* editorpane.c — GtkSourceView 5 editor with centered ~72ch writing column,
 * incremental per-paragraph live-preview Markdown GtkTextTags, visual-only
 * blockquote / ## AI draft accents, 1s debounced autosave, and Ctrl+Click wiki-links. */
#include "editorpane.h"
#include "frontmatter.h"
#include "linkresolve.h"
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
  GtkTextTag *t_link_broken;
  GtkTextTag *t_link_blocked;
  GtkTextTag *t_link_hover;
  GtkTextTag *t_tag;
  GtkTextTag *t_frontmatter;
  GtkTextTag *t_ai_heading;
  GtkTextTag *t_ai_body;
  GtkTextTag *t_math;
  GtkTextTag *t_anchor_flash;
  guint anchor_flash_id;

  BtEditorSaveFn on_save;
  BtEditorLinkFn on_link;
  BtEditorDirtyFn on_dirty;
  BtEditorDiffFn on_diff;
  BtEditorReloadFn on_reload;
  gpointer user_data;

  /* Incremental link span model (L2-1, sorted by byte offset). */
  GArray *link_spans;
  BtIndexSnapshot *snapshot;
  const BtVault *vault;
  gint pending_del_char;
  gsize pending_del_bytes;
  gint pending_del_nl;
  gboolean pending_del_structural;

  /* Click gesture & ADR-016 state (L2-2). */
  gboolean links_open_on_click;
  gboolean press_active;
  guint press_button;
  gint press_n_press;
  gdouble press_x;
  gdouble press_y;
  GdkModifierType press_mods;
  gboolean press_had_sel;
  gsize press_caret_byte;
  gboolean press_has_span;
  gsize press_span_start;
  gsize press_span_end;

  /* Hover, cursor & transition state (L2-3). */
  gboolean ctrl_held;
  gboolean hover_active;
  gdouble hover_x;
  gdouble hover_y;
  gsize hover_span_start;
  gsize hover_span_end;
  guint32 hover_base_rgb;
  gint64 hover_start_us;
  guint hover_anim_id;
  gdouble last_click_x;
  gdouble last_click_y;
  gboolean has_last_click;

  /* Scroll position tracking & asynchronous restoration (L2-6). */
  gdouble tracked_scroll_y;
  gdouble pending_scroll_y;
  gboolean has_pending_scroll;
  guint scroll_idle_id;
};

/* ----------------------------------------------------- autosave & dirty -- */

static void cancel_autosave(BtEditorPane *pane)
{
  if (pane->autosave_id != 0) {
    g_source_remove(pane->autosave_id);
    pane->autosave_id = 0;
  }
}

static gboolean emit_save(BtEditorPane *pane)
{
  if (pane->relpath == NULL || pane->on_save == NULL)
    return TRUE;
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &start, &end);
  gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &start, &end, TRUE);
  gboolean ok = pane->on_save(pane->relpath, text, strlen(text), pane->user_data);
  g_free(text);
  return ok;
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
  p->t_link_broken = gtk_text_buffer_create_tag(b, "bt-link-broken",
      "foreground", "#D4A12A", "underline", PANGO_UNDERLINE_ERROR, NULL);
  p->t_link_blocked = gtk_text_buffer_create_tag(b, "bt-link-blocked",
      "foreground", "#D96B6B", "underline", PANGO_UNDERLINE_DOUBLE, NULL);
  p->t_link_hover = gtk_text_buffer_create_tag(b, "bt-link-hover",
      "foreground", "#A79BFA", NULL);
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
  p->t_anchor_flash = gtk_text_buffer_create_tag(b, "bt-anchor-flash",
      "paragraph-background", "#2A2447", NULL);
  gtk_text_tag_set_priority(p->t_link_hover,
                            gtk_text_tag_table_get_size(gtk_text_buffer_get_tag_table(b)) - 1);
}

static void clear_md_tags(BtEditorPane *p, const GtkTextIter *a, const GtkTextIter *b)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  GtkTextTag *all[] = {
    p->t_h1, p->t_h2, p->t_h3, p->t_syntax, p->t_bold, p->t_italic,
    p->t_code, p->t_codeblock, p->t_list_marker, p->t_quote,
    p->t_wikilink, p->t_link_broken, p->t_link_blocked, p->t_link_hover,
    p->t_tag, p->t_frontmatter, p->t_ai_heading, p->t_ai_body,
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
  GArray *mspans = NULL;
  if (strchr(s, '$') != NULL || strchr(s, '\\') != NULL) {
    BtMathScanOpts mopts = bt_math_scan_default_opts();
    mopts.skip_frontmatter = FALSE;
    mspans = bt_math_scan(s, (gsize)len, &mopts);
  }
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
    /* Wiki-link: [[target]] (keep delimiter/emphasis protection; status tags applied from link_spans) */
    if (s[i] == '[' && i + 1 < len && s[i + 1] == '[') {
      const gchar *close = strstr(s + i + 2, "]]");
      if (close != NULL && close > s + i + 2 &&
          find_math_span_at(mspans, (gsize)(close - s)) == NULL) {
        gint end = (gint)(close - s);
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
  if (mspans != NULL)
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
  if (!p->loading)
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

static gboolean has_link_structural_token(const gchar *txt, gsize len)
{
  if (txt == NULL || len == 0)
    return FALSE;
  g_autofree gchar *tmp = g_strndup(txt, len);
  return strstr(tmp, "```") != NULL || strstr(tmp, "~~~") != NULL ||
         strstr(tmp, "$$") != NULL || strstr(tmp, "---") != NULL ||
         strstr(tmp, "<!--") != NULL || strstr(tmp, "-->") != NULL ||
         strchr(tmp, '`') != NULL;
}

static gsize count_nl_bytes(const gchar *s, gsize len)
{
  gsize n = 0;
  for (gsize i = 0; i < len; i++)
    if (s[i] == '\n')
      n++;
  return n;
}

static void expand_to_neighbor_paragraphs(const gchar *text, gsize len,
                                          gsize *io_start, gsize *io_end)
{
  /* Walk backwards across one preceding blank-line boundary so previous neighbor
   * paragraph is included in the scan window (A.2). */
  gsize s = *io_start;
  gboolean saw_blank = FALSE;
  while (s > 0) {
    gsize line_end = s;
    gsize line_start = line_end;
    while (line_start > 0 && text[line_start - 1] != '\n')
      line_start--;
    gboolean blank = TRUE;
    for (gsize k = line_start; k < line_end; k++) {
      if (text[k] != ' ' && text[k] != '\t' && text[k] != '\r') {
        blank = FALSE;
        break;
      }
    }
    if (blank) {
      if (saw_blank)
        break;
      saw_blank = TRUE;
    } else if (saw_blank) {
      /* We stepped back into the start of the previous neighbor paragraph; keep stepping
       * to its own beginning, then stop on the blank line before it. */
    }
    if (line_start == 0) {
      s = 0;
      break;
    }
    s = line_start - 1;
  }
  *io_start = s;

  /* Walk forwards across the next blank-line boundary so the following neighbor
   * paragraph is included as well. */
  gsize e = *io_end;
  saw_blank = FALSE;
  while (e < len) {
    gsize line_start = e;
    gsize line_end = line_start;
    while (line_end < len && text[line_end] != '\n')
      line_end++;
    gboolean blank = TRUE;
    for (gsize k = line_start; k < line_end; k++) {
      if (text[k] != ' ' && text[k] != '\t' && text[k] != '\r') {
        blank = FALSE;
        break;
      }
    }
    if (blank) {
      if (saw_blank)
        break;
      saw_blank = TRUE;
    }
    e = (line_end < len) ? (line_end + 1) : len;
  }
  *io_end = e;
}

static BtLinkSpan copy_link_span(const BtLinkSpan *src)
{
  BtLinkSpan c = *src;
  c.raw_target = g_strdup(src->raw_target);
  c.md_label = g_strdup(src->md_label);
  return c;
}

static void update_link_spans_incremental(BtEditorPane *p, gsize edit_byte, gsize old_bytes,
                                          gsize new_bytes, gint nl_delta, gboolean structural)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  GtkTextIter bstart, bend;
  gtk_text_buffer_get_bounds(buf, &bstart, &bend);
  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &bstart, &bend, TRUE);
  gsize len = strlen(txt);

  if (p->link_spans == NULL) {
    p->link_spans = bt_link_scan(txt, len);
    return;
  }

  gsize rstart = edit_byte;
  gsize rend = MIN(edit_byte + new_bytes, len);
  if (structural || edit_byte < 256u) {
    rstart = 0;
    rend = len;
  } else {
    expand_to_neighbor_paragraphs(txt, len, &rstart, &rend);
  }

  gsize block_start = 0, block_end = len;
  GArray *fresh = bt_link_scan_range(txt, len, rstart, rend, &block_start, &block_end);

  /* Ensure block boundaries cover the edit window in post-edit coordinates. */
  if (block_start > edit_byte)
    block_start = 0;
  if (block_end < MIN(edit_byte + new_bytes, len))
    block_end = len;

  gssize delta = (gssize)new_bytes - (gssize)old_bytes;
  gsize pre_block_end = (gsize)MAX((gssize)block_end - delta, 0);

  GArray *merged = g_array_new(FALSE, FALSE, sizeof(BtLinkSpan));
  g_array_set_clear_func(merged, (GDestroyNotify) NULL); /* manual ownership below */

  guint i = 0;
  while (i < p->link_spans->len) {
    const BtLinkSpan *os = &g_array_index(p->link_spans, BtLinkSpan, i);
    if (os->end <= block_start) {
      BtLinkSpan cp = copy_link_span(os);
      g_array_append_val(merged, cp);
      i++;
    } else {
      break;
    }
  }
  while (i < p->link_spans->len) {
    const BtLinkSpan *os = &g_array_index(p->link_spans, BtLinkSpan, i);
    if (os->start < pre_block_end || os->start < edit_byte + old_bytes)
      i++;
    else
      break;
  }
  for (guint j = 0; j < fresh->len; j++) {
    BtLinkSpan cp = copy_link_span(&g_array_index(fresh, BtLinkSpan, j));
    g_array_append_val(merged, cp);
  }
  while (i < p->link_spans->len) {
    const BtLinkSpan *os = &g_array_index(p->link_spans, BtLinkSpan, i);
    BtLinkSpan cp = copy_link_span(os);
    cp.start = (gsize)((gssize)cp.start + delta);
    cp.end = (gsize)((gssize)cp.end + delta);
    cp.target_start = (gsize)((gssize)cp.target_start + delta);
    cp.target_end = (gsize)((gssize)cp.target_end + delta);
    cp.line_no = (guint)MAX((gint)cp.line_no + nl_delta, 1);
    g_array_append_val(merged, cp);
    i++;
  }

  g_array_unref(fresh);
  /* Transfer clear_func by freeing old link_spans and setting clear_func on merged. */
  for (guint k = 0; k < merged->len; k++) {
    /* already valid owned strings */
    (void)k;
  }
  g_array_unref(p->link_spans);
  /* Build final array with clear_func from bt_link_scan-compatible array */
  GArray *final_arr = bt_link_scan("", 0);
  for (guint k = 0; k < merged->len; k++)
    g_array_append_val(final_arr, g_array_index(merged, BtLinkSpan, k));
  g_array_free(merged, TRUE);
  p->link_spans = final_arr;
}

static gboolean raw_target_is_placeholder(const char *raw_target);

static BtLinkResolution *resolve_span_for_pane(const BtEditorPane *pane, const BtLinkSpan *sp)
{
  if (pane == NULL || sp == NULL || sp->raw_target == NULL)
    return NULL;
  if (raw_target_is_placeholder(sp->raw_target)) {
    BtLinkResolution *r = g_new0(BtLinkResolution, 1);
    r->status = BT_LINK_PLACEHOLDER;
    r->kind = BT_LINK_KIND_NOTE;
    r->display = g_strdup(sp->raw_target);
    return r;
  }
  if (pane->snapshot != NULL)
    return bt_link_resolve(pane->snapshot, pane->relpath, sp->raw_target);
  return NULL;
}

static GtkTextTag *tag_for_span(const BtEditorPane *pane, const BtLinkSpan *sp,
                                gboolean *out_is_placeholder, guint32 *out_rgb)
{
  *out_is_placeholder = FALSE;
  if (raw_target_is_placeholder(sp->raw_target)) {
    *out_is_placeholder = TRUE;
    if (out_rgb) *out_rgb = BT_COL_TEXT_PRIMARY;
    return NULL;
  }
  if (pane->snapshot == NULL) {
    if (out_rgb) *out_rgb = BT_COL_LINK;
    return pane->t_wikilink;
  }
  BtLinkResolution *res = bt_link_resolve(pane->snapshot, pane->relpath, sp->raw_target);
  if (res == NULL) {
    if (out_rgb) *out_rgb = BT_COL_LINK;
    return pane->t_wikilink;
  }
  if (res->status == BT_LINK_PLACEHOLDER) {
    bt_link_resolution_free(res);
    *out_is_placeholder = TRUE;
    if (out_rgb) *out_rgb = BT_COL_TEXT_PRIMARY;
    return NULL;
  }
  BtLinkOpenPolicy pol;
  bt_link_check_open_policy(res, NULL, TRUE, &pol);
  GtkTextTag *tag = pane->t_wikilink;
  guint32 rgb = BT_COL_LINK;
  if (res->status == BT_LINK_BLOCKED || pol.action == BT_LINK_OPEN_BLOCKED) {
    tag = pane->t_link_blocked;
    rgb = BT_COL_LINK_BLOCKED;
  } else if (res->status == BT_LINK_UNRESOLVED) {
    tag = pane->t_link_broken;
    rgb = BT_COL_LINK_BROKEN;
  }
  bt_link_open_policy_clear(&pol);
  bt_link_resolution_free(res);
  if (out_rgb) *out_rgb = rgb;
  return tag;
}

static void apply_link_tags_range_inner(BtEditorPane *p, gsize byte_lo, gsize byte_hi)
{
  if (p == NULL || p->buffer == NULL || p->link_spans == NULL)
    return;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(p->buffer);
  GtkTextIter bstart, bend;
  gtk_text_buffer_get_bounds(buf, &bstart, &bend);
  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &bstart, &bend, TRUE);
  gsize total = strlen(txt);
  if (byte_hi > total)
    byte_hi = total;
  if (byte_lo > byte_hi)
    byte_lo = byte_hi;

  gint c_lo = (gint)g_utf8_strlen(txt, (gssize)byte_lo);
  gint c_hi = c_lo + (gint)g_utf8_strlen(txt + byte_lo, (gssize)(byte_hi - byte_lo));
  GtkTextIter it_lo, it_hi;
  gtk_text_buffer_get_iter_at_offset(buf, &it_lo, c_lo);
  gtk_text_buffer_get_iter_at_offset(buf, &it_hi, c_hi);

  if (!p->loading) {
    gtk_text_buffer_remove_tag(buf, p->t_wikilink, &it_lo, &it_hi);
    gtk_text_buffer_remove_tag(buf, p->t_link_broken, &it_lo, &it_hi);
    gtk_text_buffer_remove_tag(buf, p->t_link_blocked, &it_lo, &it_hi);
  }

  /* Binary search for first span overlapping [byte_lo, byte_hi). */
  guint lo = 0, hi = p->link_spans->len;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    const BtLinkSpan *ms = &g_array_index(p->link_spans, BtLinkSpan, mid);
    if (ms->end <= byte_lo)
      lo = mid + 1;
    else
      hi = mid;
  }

  gsize cur_b = 0;
  gint cur_c = 0;
  for (guint i = lo; i < p->link_spans->len; i++) {
    const BtLinkSpan *sp = &g_array_index(p->link_spans, BtLinkSpan, i);
    if (sp->start >= byte_hi)
      break;
    if (sp->end > total)
      continue;

    gboolean is_placeholder = FALSE;
    GtkTextTag *ltag = tag_for_span(p, sp, &is_placeholder, NULL);

    /* Advance O(1)-amortized UTF-8 char cursor cleanly. */
    if (sp->start < cur_b) {
      cur_b = 0;
      cur_c = 0;
    }
    cur_c += (gint)g_utf8_strlen(txt + cur_b, (gssize)(sp->start - cur_b));
    cur_b = sp->start;
    gint s_c = cur_c;
    gint e_c = s_c + (gint)g_utf8_strlen(txt + sp->start, (gssize)(sp->end - sp->start));

    GtkTextIter s_it, e_it;
    gtk_text_buffer_get_iter_at_offset(buf, &s_it, s_c);
    gtk_text_buffer_get_iter_at_offset(buf, &e_it, e_c);

    if (is_placeholder || ltag == NULL) {
      gtk_text_buffer_remove_tag(buf, p->t_syntax, &s_it, &e_it);
      continue;
    }

    if (sp->syntax == BT_LINK_SYNTAX_WIKILINK) {
      gint ts_c = s_c + (gint)g_utf8_strlen(txt + sp->start, (gssize)(sp->target_start - sp->start));
      gint te_c = s_c + (gint)g_utf8_strlen(txt + sp->start, (gssize)(sp->target_end - sp->start));
      GtkTextIter ts_it, te_it;
      gtk_text_buffer_get_iter_at_offset(buf, &ts_it, ts_c);
      gtk_text_buffer_get_iter_at_offset(buf, &te_it, te_c);
      gtk_text_buffer_apply_tag(buf, p->t_syntax, &s_it, &ts_it);
      gtk_text_buffer_apply_tag(buf, ltag, &ts_it, &te_it);
      gtk_text_buffer_apply_tag(buf, p->t_syntax, &te_it, &e_it);
    } else if (sp->syntax == BT_LINK_SYNTAX_MARKDOWN) {
      /* Style visible label and destination delimiters; keep entire span hoverable. */
      const char *close_br = strstr(txt + sp->start + 1, "](");
      gsize lbl_end_b = close_br ? (gsize)(close_br - txt) : sp->target_start;
      gsize lb_b = MIN(sp->start + 1, lbl_end_b);
      gint l0_c = s_c + (gint)g_utf8_strlen(txt + sp->start, (gssize)(lb_b - sp->start));
      gint l1_c = s_c + (gint)g_utf8_strlen(txt + sp->start, (gssize)(lbl_end_b - sp->start));
      GtkTextIter l0_it, l1_it;
      gtk_text_buffer_get_iter_at_offset(buf, &l0_it, l0_c);
      gtk_text_buffer_get_iter_at_offset(buf, &l1_it, l1_c);
      gtk_text_buffer_apply_tag(buf, p->t_syntax, &s_it, &l0_it);
      gtk_text_buffer_apply_tag(buf, ltag, &l0_it, &l1_it);
      gtk_text_buffer_apply_tag(buf, p->t_syntax, &l1_it, &e_it);
    } else {
      gtk_text_buffer_apply_tag(buf, ltag, &s_it, &e_it);
    }
  }
}

static void apply_link_tags_range(BtEditorPane *p, gsize byte_lo, gsize byte_hi)
{
  if (p == NULL)
    return;
  if (p->snapshot != NULL)
    bt_index_snapshot_set_fast_tag_mode(p->snapshot, TRUE);
  apply_link_tags_range_inner(p, byte_lo, byte_hi);
  if (p->snapshot != NULL)
    bt_index_snapshot_set_fast_tag_mode(p->snapshot, FALSE);
}

static gsize iter_byte_in_buffer(GtkTextBuffer *buf, const GtkTextIter *it)
{
  GtkTextIter bstart;
  gtk_text_buffer_get_start_iter(buf, &bstart);
  g_autofree gchar *prefix = gtk_text_buffer_get_text(buf, &bstart, it, TRUE);
  return strlen(prefix);
}

static void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *loc, gchar *text, gint len,
                                 gpointer ud)
{
  BtEditorPane *p = ud;
  if (p->loading)
    return;
  gint end_ln = gtk_text_iter_get_line(loc);
  gint newlines = (gint)count_nl_bytes(text, (gsize)MAX(len, 0));
  gint start_ln = MAX(end_ln - newlines, 0);

  /* Fence/heading/frontmatter/display-math edits can affect subsequent lines. */
  gboolean structural = (start_ln == 0) || strstr(text, "```") != NULL ||
                        strstr(text, "---") != NULL || strstr(text, "##") != NULL ||
                        strstr(text, "$$") != NULL;
  if (structural)
    retag_lines(p, start_ln, gtk_text_buffer_get_line_count(buf) - 1);
  else
    retag_lines(p, start_ln, end_ln);

  gsize end_byte = iter_byte_in_buffer(buf, loc);
  gsize ulen = (gsize)MAX(len, 0);
  gsize edit_byte = (end_byte >= ulen) ? (end_byte - ulen) : 0;
  gboolean link_struct = has_link_structural_token(text, ulen);
  update_link_spans_incremental(p, edit_byte, 0, ulen, newlines, link_struct);
  GtkTextIter line_it;
  gtk_text_buffer_get_iter_at_line(buf, &line_it, start_ln);
  gsize r0 = iter_byte_in_buffer(buf, &line_it);
  gsize r1 = MAX(end_byte + 1, r0);
  if (structural || link_struct) {
    r0 = 0;
    r1 = G_MAXSIZE;
  }
  apply_link_tags_range(p, r0, r1);
}

static void on_delete_range_before(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end,
                                   gpointer ud)
{
  BtEditorPane *p = ud;
  if (p->loading)
    return;
  p->pending_del_char = gtk_text_iter_get_offset(start);
  g_autofree gchar *del_txt = gtk_text_buffer_get_text(buf, start, end, TRUE);
  p->pending_del_bytes = strlen(del_txt);
  p->pending_del_nl = (gint)count_nl_bytes(del_txt, p->pending_del_bytes);
  p->pending_del_structural = has_link_structural_token(del_txt, p->pending_del_bytes);
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

  GtkTextIter del_it;
  gtk_text_buffer_get_iter_at_offset(buf, &del_it, p->pending_del_char);
  gsize edit_byte = iter_byte_in_buffer(buf, &del_it);
  gboolean was_struct = p->pending_del_structural;
  update_link_spans_incremental(p, edit_byte, p->pending_del_bytes, 0,
                                -p->pending_del_nl, p->pending_del_structural);
  if (was_struct)
    apply_link_tags_range(p, 0, G_MAXSIZE);
  else {
    GtkTextIter l_it;
    gtk_text_buffer_get_iter_at_line(buf, &l_it, ln);
    apply_link_tags_range(p, iter_byte_in_buffer(buf, &l_it), edit_byte + 1);
  }
  p->pending_del_bytes = 0;
  p->pending_del_nl = 0;
  p->pending_del_structural = FALSE;
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

static gboolean apply_pending_scroll(BtEditorPane *p)
{
  if (p == NULL || !p->has_pending_scroll || p->scrolled == NULL)
    return TRUE;
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->scrolled));
  if (vadj == NULL)
    return TRUE;
  gdouble upper = gtk_adjustment_get_upper(vadj);
  gdouble page = gtk_adjustment_get_page_size(vadj);
  if (p->pending_scroll_y <= 0.0) {
    gtk_adjustment_set_value(vadj, 0.0);
    p->tracked_scroll_y = 0.0;
    p->has_pending_scroll = FALSE;
    return TRUE;
  }
  if (upper > page) {
    gdouble clamped = CLAMP(p->pending_scroll_y, 0.0, upper - page);
    gtk_adjustment_set_value(vadj, clamped);
    p->tracked_scroll_y = clamped;
    if (upper - page + 0.5 >= p->pending_scroll_y) {
      p->has_pending_scroll = FALSE;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean on_scroll_idle(gpointer ud)
{
  BtEditorPane *p = ud;
  p->scroll_idle_id = 0;
  apply_pending_scroll(p);
  return G_SOURCE_REMOVE;
}

static void on_vadj_changed(GtkAdjustment *adj, gpointer ud)
{
  (void)adj;
  apply_pending_scroll((BtEditorPane *)ud);
}

static void on_vscroll_changed(GtkAdjustment *adj, gpointer ud)
{
  BtEditorPane *p = ud;
  if (!p->has_pending_scroll && adj != NULL)
    p->tracked_scroll_y = gtk_adjustment_get_value(adj);
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

static gboolean raw_target_is_placeholder(const char *raw_target)
{
  if (raw_target == NULL)
    return FALSE;
  const char *open = strstr(raw_target, "{{");
  if (open == NULL)
    return FALSE;
  const char *p = open + 2;
  while (*p != '\0' && *p != '}' && *p != '\r' && *p != '\n')
    p++;
  return p[0] == '}' && p[1] == '}';
}

static gsize caret_byte_in_buffer(const BtEditorPane *pane)
{
  if (pane == NULL || pane->buffer == NULL)
    return 0;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(buf, &it, gtk_text_buffer_get_insert(buf));
  return iter_byte_in_buffer(buf, &it);
}

static void place_caret_byte_in_buffer(BtEditorPane *pane, gsize byte_off)
{
  if (pane == NULL || pane->buffer == NULL)
    return;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  GtkTextIter s, e, target;
  gtk_text_buffer_get_bounds(buf, &s, &e);
  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
  gsize total = strlen(txt);
  if (byte_off > total)
    byte_off = total;
  gint char_off = (gint)g_utf8_strlen(txt, (gssize)byte_off);
  gtk_text_buffer_get_iter_at_offset(buf, &target, char_off);
  gtk_text_buffer_place_cursor(buf, &target);
}

static gboolean hit_test_link_at_coords(const BtEditorPane *pane, gdouble x, gdouble y,
                                        BtLinkSpan *out_span)
{
  if (pane == NULL || pane->view == NULL || pane->link_spans == NULL)
    return FALSE;
  gint bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(pane->view), GTK_TEXT_WINDOW_WIDGET,
                                        (gint)x, (gint)y, &bx, &by);
  GtkTextIter iter;
  gint trailing = 0;
  if (!gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(pane->view), &iter, &trailing, bx, by))
    return FALSE;
  if (gtk_text_iter_ends_line(&iter))
    return FALSE;

  GdkRectangle ch_rect;
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(pane->view), &iter, &ch_rect);
  if (by < ch_rect.y || by > ch_rect.y + ch_rect.height)
    return FALSE;
  if (bx < ch_rect.x - 1 || bx > ch_rect.x + MAX(ch_rect.width, 4) + 2)
    return FALSE;

  gsize byte_off = iter_byte_in_buffer(GTK_TEXT_BUFFER(pane->buffer), &iter);
  BtLinkSpan hit;
  if (!bt_editorpane_link_at_offset(pane, byte_off, &hit))
    return FALSE;
  if (raw_target_is_placeholder(hit.raw_target))
    return FALSE;
  if (out_span != NULL)
    *out_span = hit;
  return TRUE;
}

static gboolean evaluate_and_fire_link_click(BtEditorPane *pane, guint button, gint n_press,
                                             gdouble press_x, gdouble press_y,
                                             gdouble rel_x, gdouble rel_y,
                                             gboolean press_had_sel, GdkModifierType mods,
                                             gsize pre_caret_byte, const BtLinkSpan *span)
{
  if (pane == NULL || pane->relpath == NULL || span == NULL)
    return FALSE;
  if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_MIDDLE)
    return FALSE;
  if (n_press != 1)
    return FALSE;
  if (pane->view != NULL &&
      gtk_drag_check_threshold(GTK_WIDGET(pane->view), (gint)press_x, (gint)press_y,
                               (gint)rel_x, (gint)rel_y)) {
    return FALSE;
  }
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  if (press_had_sel || gtk_text_buffer_get_has_selection(buf))
    return FALSE;
  if ((mods & GDK_ALT_MASK) != 0)
    return FALSE;
  if (raw_target_is_placeholder(span->raw_target))
    return FALSE;

  /* Caret-inside rule (ADR-016): do not open if caret was already inside this span. */
  if (pre_caret_byte > span->start && pre_caret_byte < span->end)
    return FALSE;

  /* Setting links_open_on_click: Ctrl+Click opens in both modes; plain click only when enabled. */
  if (button == GDK_BUTTON_PRIMARY && !pane->links_open_on_click &&
      (mods & GDK_CONTROL_MASK) == 0) {
    return FALSE;
  }

  /* Consume click without moving the caret in the source note (ADR-016). */
  pane->last_click_x = rel_x;
  pane->last_click_y = rel_y;
  pane->has_last_click = TRUE;
  place_caret_byte_in_buffer(pane, pre_caret_byte);
  if (pane->on_link != NULL && span->raw_target != NULL)
    pane->on_link(span->raw_target, pane->user_data);
  return TRUE;
}

static void on_editor_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                              gpointer ud)
{
  BtEditorPane *pane = ud;
  pane->press_active = FALSE;
  pane->press_has_span = FALSE;
  if (pane->relpath == NULL || pane->buffer == NULL)
    return;

  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_MIDDLE)
    return;

  GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  pane->press_active = TRUE;
  pane->press_button = button;
  pane->press_n_press = n_press;
  pane->press_x = x;
  pane->press_y = y;
  pane->press_mods = ev ? gdk_event_get_modifier_state(ev) : gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  pane->press_had_sel = gtk_text_buffer_get_has_selection(GTK_TEXT_BUFFER(pane->buffer));
  pane->press_caret_byte = caret_byte_in_buffer(pane);

  BtLinkSpan sp;
  if (hit_test_link_at_coords(pane, x, y, &sp)) {
    pane->press_has_span = TRUE;
    pane->press_span_start = sp.start;
    pane->press_span_end = sp.end;
  }
}

static void on_editor_released(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                               gpointer ud)
{
  BtEditorPane *pane = ud;
  gboolean was_active = pane->press_active;
  pane->press_active = FALSE;
  if (!was_active || !pane->press_has_span)
    return;

  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  GdkModifierType rel_mods =
      (ev ? gdk_event_get_modifier_state(ev)
          : gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture))) |
      pane->press_mods;

  BtLinkSpan rel_sp;
  if (!hit_test_link_at_coords(pane, x, y, &rel_sp))
    return;
  if (rel_sp.start != pane->press_span_start || rel_sp.end != pane->press_span_end)
    return;

  gint eff_press = MAX(n_press, pane->press_n_press);
  if (evaluate_and_fire_link_click(pane, button, eff_press, pane->press_x, pane->press_y,
                                   x, y, pane->press_had_sel, rel_mods,
                                   pane->press_caret_byte, &rel_sp)) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }
}

static void on_editor_stopped(GtkGestureClick *gesture, gpointer ud)
{
  (void)gesture;
  BtEditorPane *pane = ud;
  pane->press_active = FALSE;
  pane->press_has_span = FALSE;
}

/* ------------------------------------------- hover, cursor & tooltips -- */

static gboolean editor_reduce_motion(const BtEditorPane *pane)
{
  const char *env = g_getenv("BT_REDUCE_MOTION");
  if (env != NULL && strcmp(env, "1") == 0)
    return TRUE;
  if (pane == NULL || pane->view == NULL)
    return TRUE;
  GtkSettings *st = gtk_widget_get_settings(GTK_WIDGET(pane->view));
  if (st == NULL)
    return TRUE;
  gboolean anim = TRUE;
  g_object_get(st, "gtk-enable-animations", &anim, NULL);
  return !anim;
}

static void apply_hover_tag_fraction(BtEditorPane *pane, double t)
{
  if (pane == NULL || pane->buffer == NULL || !pane->hover_active)
    return;
  gdouble r0, g0, b0, r1, g1, b1;
  bt_theme_unpack(pane->hover_base_rgb, &r0, &g0, &b0);
  bt_theme_unpack(BT_COL_LINK_HOVER, &r1, &g1, &b1);
  GdkRGBA col = {
    .red = (float)(r0 + (r1 - r0) * t),
    .green = (float)(g0 + (g1 - g0) * t),
    .blue = (float)(b0 + (b1 - b0) * t),
    .alpha = 1.0f,
  };
  g_object_set(pane->t_link_hover, "foreground-rgba", &col, NULL);

  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  GtkTextIter b0_it, b1_it;
  gtk_text_buffer_get_bounds(buf, &b0_it, &b1_it);
  gtk_text_buffer_remove_tag(buf, pane->t_link_hover, &b0_it, &b1_it);

  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &b0_it, &b1_it, TRUE);
  gsize total = strlen(txt);
  gsize hs = MIN(pane->hover_span_start, total);
  gsize he = MIN(pane->hover_span_end, total);
  if (he > hs) {
    GtkTextIter s_it, e_it;
    gint cs = (gint)g_utf8_strlen(txt, (gssize)hs);
    gint ce = cs + (gint)g_utf8_strlen(txt + hs, (gssize)(he - hs));
    gtk_text_buffer_get_iter_at_offset(buf, &s_it, cs);
    gtk_text_buffer_get_iter_at_offset(buf, &e_it, ce);
    gtk_text_buffer_apply_tag(buf, pane->t_link_hover, &s_it, &e_it);
  }
}

static gboolean on_hover_anim_tick(gpointer ud)
{
  BtEditorPane *pane = ud;
  if (!pane->hover_active) {
    pane->hover_anim_id = 0;
    return G_SOURCE_REMOVE;
  }
  double elapsed_ms = (double)(g_get_monotonic_time() - pane->hover_start_us) / 1000.0;
  double t = CLAMP(elapsed_ms / 120.0, 0.0, 1.0);
  apply_hover_tag_fraction(pane, t);
  if (t >= 1.0) {
    pane->hover_anim_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void clear_hover_state(BtEditorPane *pane)
{
  if (pane == NULL)
    return;
  if (pane->hover_anim_id != 0) {
    g_source_remove(pane->hover_anim_id);
    pane->hover_anim_id = 0;
  }
  pane->hover_active = FALSE;
  pane->hover_span_start = 0;
  pane->hover_span_end = 0;
  if (pane->buffer != NULL && pane->t_link_hover != NULL) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &s, &e);
    gtk_text_buffer_remove_tag(GTK_TEXT_BUFFER(pane->buffer), pane->t_link_hover, &s, &e);
  }
  if (pane->view != NULL && GTK_IS_WIDGET(pane->view))
    gtk_widget_set_cursor_from_name(GTK_WIDGET(pane->view), "text");
}

static void update_hover_at_coords(BtEditorPane *pane, gdouble x, gdouble y)
{
  if (pane == NULL || pane->view == NULL)
    return;
  pane->hover_x = x;
  pane->hover_y = y;
  BtLinkSpan sp;
  if (!hit_test_link_at_coords(pane, x, y, &sp)) {
    clear_hover_state(pane);
    return;
  }

  gboolean want_pointer = pane->links_open_on_click || pane->ctrl_held;
  gtk_widget_set_cursor_from_name(GTK_WIDGET(pane->view), want_pointer ? "pointer" : "text");

  gboolean same = pane->hover_active &&
                  pane->hover_span_start == sp.start &&
                  pane->hover_span_end == sp.end;
  if (same)
    return;

  clear_hover_state(pane);
  gtk_widget_set_cursor_from_name(GTK_WIDGET(pane->view), want_pointer ? "pointer" : "text");

  gboolean is_placeholder = FALSE;
  guint32 base_rgb = BT_COL_LINK;
  tag_for_span(pane, &sp, &is_placeholder, &base_rgb);
  if (is_placeholder)
    return;

  pane->hover_active = TRUE;
  pane->hover_span_start = sp.start;
  pane->hover_span_end = sp.end;
  pane->hover_base_rgb = base_rgb;

  if (editor_reduce_motion(pane)) {
    apply_hover_tag_fraction(pane, 1.0);
  } else {
    pane->hover_start_us = g_get_monotonic_time();
    apply_hover_tag_fraction(pane, 0.25);
    pane->hover_anim_id = g_timeout_add(16, on_hover_anim_tick, pane);
  }
}

static void on_editor_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer ud)
{
  BtEditorPane *pane = ud;
  GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(ctrl));
  pane->ctrl_held = (mods & GDK_CONTROL_MASK) != 0;
  update_hover_at_coords(pane, x, y);
}

static void on_editor_leave(GtkEventControllerMotion *ctrl, gpointer ud)
{
  (void)ctrl;
  clear_hover_state(ud);
}

static gboolean on_editor_key_event(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                                    GdkModifierType state, gpointer ud)
{
  (void)ctrl; (void)keycode;
  BtEditorPane *pane = ud;
  gboolean now_ctrl = (state & GDK_CONTROL_MASK) != 0 ||
                      keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R;
  if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R) {
    /* key-released updates cleared mask via current state below if needed */
  }
  if (pane->hover_active && now_ctrl != pane->ctrl_held) {
    pane->ctrl_held = now_ctrl;
    gboolean want_pointer = pane->links_open_on_click || pane->ctrl_held;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(pane->view), want_pointer ? "pointer" : "text");
  }
  return FALSE;
}

static void on_editor_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                                   GdkModifierType state, gpointer ud)
{
  (void)ctrl; (void)keycode; (void)keyval;
  BtEditorPane *pane = ud;
  pane->ctrl_held = (state & GDK_CONTROL_MASK) != 0;
  if (pane->hover_active) {
    gboolean want_pointer = pane->links_open_on_click || pane->ctrl_held;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(pane->view), want_pointer ? "pointer" : "text");
  }
}

static gboolean offset_is_in_ai_draft(const BtEditorPane *pane, gsize byte_offset)
{
  if (pane == NULL || pane->buffer == NULL)
    return FALSE;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(buf, &s, &e);
  g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
  gsize total = strlen(txt);
  if (byte_offset > total)
    byte_offset = total;
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_offset(buf, &it, (gint)g_utf8_strlen(txt, (gssize)byte_offset));
  return gtk_text_iter_has_tag(&it, pane->t_ai_body) ||
         gtk_text_iter_has_tag(&it, pane->t_ai_heading);
}

gchar *bt_editorpane_tooltip_for_offset(const BtEditorPane *pane, gsize byte_offset)
{
  if (pane == NULL || pane->relpath == NULL)
    return NULL;
  BtLinkSpan sp;
  if (!bt_editorpane_link_at_offset(pane, byte_offset, &sp))
    return NULL;
  if (raw_target_is_placeholder(sp.raw_target))
    return NULL;

  BtLinkResolution *res = resolve_span_for_pane(pane, &sp);
  gchar *tip = NULL;
  if (res == NULL) {
    tip = g_strdup(sp.raw_target);
  } else if (res->status == BT_LINK_PLACEHOLDER) {
    bt_link_resolution_free(res);
    return NULL;
  } else {
    BtLinkOpenPolicy pol;
    bt_link_check_open_policy(res, pane->vault, TRUE, &pol);

    if (res->status == BT_LINK_BLOCKED || pol.action == BT_LINK_OPEN_BLOCKED) {
      tip = g_strdup(pol.reason_msg ? pol.reason_msg
                                    : bt_link_block_reason_message(res->block_reason));
    } else if (res->status == BT_LINK_EXTERNAL) {
      if (pol.has_userinfo) {
        tip = g_strdup_printf("%s (Warning: embedded credentials stripped)",
                              pol.safe_display_url ? pol.safe_display_url : res->url);
      } else {
        tip = g_strdup(pol.safe_display_url ? pol.safe_display_url : res->url);
      }
    } else if (res->status == BT_LINK_AMBIGUOUS) {
      guint cnt = 0;
      while (res->candidates != NULL && res->candidates[cnt] != NULL)
        cnt++;
      tip = g_strdup_printf("%u notes match. Click to choose.", cnt);
    } else if (res->status == BT_LINK_UNRESOLVED) {
      tip = g_strdup("Note not found");
    } else if (res->kind == BT_LINK_KIND_SAME_NOTE) {
      tip = res->anchor ? g_strdup_printf("#%s", res->anchor) : g_strdup("(current note)");
    } else if (res->kind == BT_LINK_KIND_FILE) {
      tip = g_strdup_printf("%s — Opens in default app", res->path ? res->path : sp.raw_target);
    } else if (res->kind == BT_LINK_KIND_DIR) {
      tip = g_strdup_printf("%s — Opens in file manager", res->path ? res->path : sp.raw_target);
    } else {
      tip = res->anchor ? g_strdup_printf("%s#%s", res->path, res->anchor)
                        : g_strdup(res->path ? res->path : sp.raw_target);
    }
    bt_link_open_policy_clear(&pol);
    bt_link_resolution_free(res);
  }

  if (tip != NULL && offset_is_in_ai_draft(pane, byte_offset)) {
    gchar *with_ai = g_strdup_printf("%s · AI draft", tip);
    g_free(tip);
    tip = with_ai;
  }
  return tip;
}

const gchar *bt_editorpane_cursor_name_at_offset(const BtEditorPane *pane, gsize byte_offset,
                                                 GdkModifierType mods)
{
  if (pane == NULL || pane->relpath == NULL)
    return "text";
  BtLinkSpan sp;
  if (!bt_editorpane_link_at_offset(pane, byte_offset, &sp))
    return "text";
  if (raw_target_is_placeholder(sp.raw_target))
    return "text";
  gboolean allow_pointer = pane->links_open_on_click || ((mods & GDK_CONTROL_MASK) != 0);
  return allow_pointer ? "pointer" : "text";
}

static gboolean on_editor_query_tooltip(GtkWidget *w, gint x, gint y, gboolean keyboard_mode,
                                        GtkTooltip *tooltip, gpointer ud)
{
  (void)w; (void)keyboard_mode;
  BtEditorPane *pane = ud;
  BtLinkSpan sp;
  if (!hit_test_link_at_coords(pane, (gdouble)x, (gdouble)y, &sp))
    return FALSE;
  g_autofree gchar *tip = bt_editorpane_tooltip_for_offset(pane, sp.start);
  if (tip == NULL || tip[0] == '\0')
    return FALSE;
  gtk_tooltip_set_text(tooltip, tip);
  return TRUE;
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
  pane->links_open_on_click = TRUE;

  pane->buffer = gtk_source_buffer_new(NULL);
  configure_language(pane->buffer);
  apply_ink_scheme(pane->buffer, TRUE);
  init_markdown_tags(pane);
  g_signal_connect(pane->buffer, "changed", G_CALLBACK(on_buffer_changed), pane);
  g_signal_connect_after(pane->buffer, "insert-text", G_CALLBACK(on_insert_text_after), pane);
  g_signal_connect(pane->buffer, "delete-range", G_CALLBACK(on_delete_range_before), pane);
  g_signal_connect_after(pane->buffer, "delete-range", G_CALLBACK(on_delete_range_after), pane);

  pane->view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(pane->buffer));
  g_object_add_weak_pointer(G_OBJECT(pane->view), (gpointer *)&pane->view);
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
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_editor_pressed), pane);
  g_signal_connect(click, "released", G_CALLBACK(on_editor_released), pane);
  g_signal_connect(click, "stopped", G_CALLBACK(on_editor_stopped), pane);
  gtk_widget_add_controller(GTK_WIDGET(pane->view), GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_editor_motion), pane);
  g_signal_connect(motion, "leave", G_CALLBACK(on_editor_leave), pane);
  gtk_widget_add_controller(GTK_WIDGET(pane->view), motion);

  GtkEventController *keyc = gtk_event_controller_key_new();
  g_signal_connect(keyc, "key-pressed", G_CALLBACK(on_editor_key_event), pane);
  g_signal_connect(keyc, "key-released", G_CALLBACK(on_editor_key_released), pane);
  gtk_widget_add_controller(GTK_WIDGET(pane->view), keyc);

  gtk_widget_set_has_tooltip(GTK_WIDGET(pane->view), TRUE);
  g_signal_connect(pane->view, "query-tooltip", G_CALLBACK(on_editor_query_tooltip), pane);

  pane->scrolled = gtk_scrolled_window_new();
  g_object_add_weak_pointer(G_OBJECT(pane->scrolled), (gpointer *)&pane->scrolled);
  gtk_widget_set_name(pane->scrolled, "bt-editor-scrolled");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pane->scrolled), GTK_WIDGET(pane->view));
  gtk_widget_set_vexpand(pane->scrolled, TRUE);
  gtk_widget_set_hexpand(pane->scrolled, TRUE);

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
  if (hadj != NULL)
    g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_scrolled_page_size), pane);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
  if (vadj != NULL) {
    g_signal_connect(vadj, "value-changed", G_CALLBACK(on_vscroll_changed), pane);
    g_signal_connect(vadj, "changed", G_CALLBACK(on_vadj_changed), pane);
  }

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

void bt_editorpane_set_links_open_on_click(BtEditorPane *pane, gboolean open_on_click)
{
  if (pane != NULL)
    pane->links_open_on_click = open_on_click;
}

gboolean bt_editorpane_get_links_open_on_click(const BtEditorPane *pane)
{
  return pane ? pane->links_open_on_click : TRUE;
}

static void clear_anchor_flash(BtEditorPane *pane)
{
  if (pane == NULL)
    return;
  if (pane->anchor_flash_id != 0) {
    g_source_remove(pane->anchor_flash_id);
    pane->anchor_flash_id = 0;
  }
  if (pane->buffer != NULL && pane->t_anchor_flash != NULL) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &s, &e);
    gtk_text_buffer_remove_tag(GTK_TEXT_BUFFER(pane->buffer), pane->t_anchor_flash, &s, &e);
  }
}

static gboolean on_anchor_flash_done(gpointer ud)
{
  BtEditorPane *pane = ud;
  pane->anchor_flash_id = 0;
  clear_anchor_flash(pane);
  return G_SOURCE_REMOVE;
}

static gchar *normalize_heading_slug(const gchar *raw)
{
  if (raw == NULL)
    return g_strdup("");
  GString *out = g_string_new(NULL);
  gboolean last_dash = TRUE;
  for (const gchar *p = raw; *p != '\0'; p = g_utf8_next_char(p)) {
    gunichar uc = g_utf8_get_char(p);
    if (g_unichar_isalnum(uc)) {
      g_string_append_unichar(out, g_unichar_tolower(uc));
      last_dash = FALSE;
    } else if (uc == ' ' || uc == '-' || uc == '_') {
      if (!last_dash) {
        g_string_append_c(out, '-');
        last_dash = TRUE;
      }
    }
  }
  while (out->len > 0 && out->str[out->len - 1] == '-')
    g_string_truncate(out, out->len - 1);
  return g_string_free(out, FALSE);
}

gboolean bt_editorpane_scroll_to_anchor(BtEditorPane *pane, const gchar *anchor, gdouble *out_target_y)
{
  if (pane == NULL || pane->buffer == NULL || anchor == NULL || anchor[0] == '\0')
    return FALSE;
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(pane->buffer);
  g_autofree gchar *want_cf = g_utf8_casefold(anchor, -1);
  g_autofree gchar *want_slug = normalize_heading_slug(anchor);

  gint nlines = gtk_text_buffer_get_line_count(buf);
  gint match_line = -1;
  for (gint ln = 0; ln < nlines; ln++) {
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line(buf, &s, ln);
    e = s;
    if (!gtk_text_iter_ends_line(&e))
      gtk_text_iter_forward_to_line_end(&e);
    g_autofree gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    const gchar *p = txt;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#')
      continue;
    while (*p == '#') p++;
    if (*p != ' ' && *p != '\t')
      continue;
    g_autofree gchar *hdr = g_strstrip(g_strdup(p));
    gsize hl = strlen(hdr);
    while (hl > 0 && (hdr[hl - 1] == '#' || hdr[hl - 1] == ' ' || hdr[hl - 1] == '\t'))
      hdr[--hl] = '\0';
    g_autofree gchar *hdr_cf = g_utf8_casefold(hdr, -1);
    g_autofree gchar *hdr_slug = normalize_heading_slug(hdr);
    if (strcmp(hdr_cf, want_cf) == 0 ||
        (want_slug[0] != '\0' && strcmp(hdr_slug, want_slug) == 0)) {
      match_line = ln;
      break;
    }
  }
  if (match_line < 0)
    return FALSE;

  GtkTextIter hs, he;
  gtk_text_buffer_get_iter_at_line(buf, &hs, match_line);
  he = hs;
  if (!gtk_text_iter_ends_line(&he))
    gtk_text_iter_forward_to_line_end(&he);
  gtk_text_buffer_place_cursor(buf, &hs);

  gdouble ty = 0.0;
  if (pane->view != NULL) {
    GdkRectangle hrect = {0}, vis = {0};
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(pane->view), &hs, &hrect);
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(pane->view), &vis);
    if (vis.height > 0)
      ty = MAX(0.0, (gdouble)hrect.y - (gdouble)vis.height / 3.0);
    else
      ty = MAX(0.0, (gdouble)hrect.y);
    if (pane->scrolled != NULL) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
      if (vadj != NULL) {
        gdouble upper = gtk_adjustment_get_upper(vadj);
        gdouble page = gtk_adjustment_get_page_size(vadj);
        if (upper > page)
          ty = CLAMP(ty, 0.0, upper - page);
        gtk_adjustment_set_value(vadj, ty);
      }
    }
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(pane->view), &hs, 0.0, TRUE, 0.0, 0.33);
    pane->tracked_scroll_y = ty;
    pane->has_pending_scroll = FALSE;
  }
  if (out_target_y != NULL)
    *out_target_y = ty;

  clear_anchor_flash(pane);
  if (!editor_reduce_motion(pane) && pane->t_anchor_flash != NULL) {
    GtkTextIter flash_end = he;
    gtk_text_iter_forward_line(&flash_end);
    gtk_text_buffer_apply_tag(buf, pane->t_anchor_flash, &hs, &flash_end);
    pane->anchor_flash_id = g_timeout_add(600, on_anchor_flash_done, pane);
  }
  return TRUE;
}

gboolean bt_editorpane_has_anchor_flash(const BtEditorPane *pane)
{
  return pane != NULL && pane->anchor_flash_id != 0;
}

void bt_editorpane_link_popover_rect(const BtEditorPane *pane, GdkRectangle *out_rect)
{
  if (out_rect == NULL)
    return;
  out_rect->x = 40;
  out_rect->y = 40;
  out_rect->width = 4;
  out_rect->height = 18;
  if (pane == NULL || pane->view == NULL)
    return;
  if (pane->has_last_click) {
    out_rect->x = (int)pane->last_click_x;
    out_rect->y = (int)pane->last_click_y;
    out_rect->width = 4;
    out_rect->height = 18;
    return;
  }
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(pane->buffer), &it,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(pane->buffer)));
  GdkRectangle brect = {0};
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(pane->view), &it, &brect);
  gint wx = 0, wy = 0;
  gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(pane->view), GTK_TEXT_WINDOW_WIDGET,
                                        brect.x, brect.y, &wx, &wy);
  out_rect->x = MAX(wx, 8);
  out_rect->y = MAX(wy, 8);
  out_rect->width = MAX(brect.width, 4);
  out_rect->height = MAX(brect.height, 16);
}

void bt_editorpane_set_index_snapshot(BtEditorPane *pane, BtIndexSnapshot *snap, const BtVault *vault)
{
  if (pane == NULL)
    return;
  pane->snapshot = snap;
  pane->vault = vault;
  if (pane->relpath != NULL)
    apply_link_tags_range(pane, 0, G_MAXSIZE);
}

void bt_editorpane_refresh_link_styles(BtEditorPane *pane)
{
  if (pane != NULL && pane->relpath != NULL)
    apply_link_tags_range(pane, 0, G_MAXSIZE);
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
  if (pane->view != NULL)
    g_object_remove_weak_pointer(G_OBJECT(pane->view), (gpointer *)&pane->view);
  if (pane->scrolled != NULL)
    g_object_remove_weak_pointer(G_OBJECT(pane->scrolled), (gpointer *)&pane->scrolled);
  clear_anchor_flash(pane);
  clear_hover_state(pane);
  cancel_autosave(pane);
  if (pane->scroll_idle_id != 0) {
    g_source_remove(pane->scroll_idle_id);
    pane->scroll_idle_id = 0;
  }
  g_clear_pointer(&pane->link_spans, g_array_unref);
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
  clear_anchor_flash(pane);
  clear_hover_state(pane);
  cancel_autosave(pane);
  if (pane->scroll_idle_id != 0) {
    g_source_remove(pane->scroll_idle_id);
    pane->scroll_idle_id = 0;
  }
  pane->has_last_click = FALSE;
  pane->tracked_scroll_y = 0.0;
  pane->pending_scroll_y = 0.0;
  pane->has_pending_scroll = FALSE;
  if (pane->scrolled != NULL) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
    if (vadj != NULL)
      gtk_adjustment_set_value(vadj, 0.0);
  }
  g_free(pane->relpath);
  pane->relpath = g_strdup(relpath);

  pane->loading = TRUE;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pane->buffer), text ? text : "", (gint)length);
  GtkTextIter bstart, bend;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(pane->buffer), &bstart, &bend);
  g_autofree gchar *buf_txt = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(pane->buffer), &bstart, &bend, TRUE);
  g_clear_pointer(&pane->link_spans, g_array_unref);
  pane->link_spans = bt_link_scan(buf_txt, strlen(buf_txt));
  gint nlines = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(pane->buffer));
  retag_lines(pane, 0, MAX(nlines - 1, 0));
  apply_link_tags_range(pane, 0, G_MAXSIZE);
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
  clear_anchor_flash(pane);
  clear_hover_state(pane);
  cancel_autosave(pane);
  if (pane->scroll_idle_id != 0) {
    g_source_remove(pane->scroll_idle_id);
    pane->scroll_idle_id = 0;
  }
  pane->has_last_click = FALSE;
  pane->tracked_scroll_y = 0.0;
  pane->pending_scroll_y = 0.0;
  pane->has_pending_scroll = FALSE;
  g_clear_pointer(&pane->relpath, g_free);
  pane->loading = TRUE;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pane->buffer), "", 0);
  g_clear_pointer(&pane->link_spans, g_array_unref);
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

gboolean bt_editorpane_flush_save(BtEditorPane *pane)
{
  if (pane == NULL || pane->relpath == NULL)
    return TRUE;
  cancel_autosave(pane);
  return emit_save(pane);
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

const GArray *bt_editorpane_link_spans(const BtEditorPane *pane)
{
  return pane ? pane->link_spans : NULL;
}

gboolean bt_editorpane_link_at_offset(const BtEditorPane *pane, gsize byte_offset, BtLinkSpan *out_span)
{
  if (pane == NULL || pane->link_spans == NULL)
    return FALSE;
  return bt_link_span_at(pane->link_spans, byte_offset, out_span);
}

gsize bt_editorpane_caret_byte_offset(const BtEditorPane *pane)
{
  return caret_byte_in_buffer(pane);
}

void bt_editorpane_set_caret_byte_offset(BtEditorPane *pane, gsize byte_offset)
{
  place_caret_byte_in_buffer(pane, byte_offset);
}

gdouble bt_editorpane_scroll_y(const BtEditorPane *pane)
{
  if (pane == NULL)
    return 0.0;
  if (pane->has_pending_scroll)
    return pane->pending_scroll_y;
  if (pane->scrolled != NULL) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
    if (vadj != NULL) {
      gdouble upper = gtk_adjustment_get_upper(vadj);
      gdouble page = gtk_adjustment_get_page_size(vadj);
      gdouble val = gtk_adjustment_get_value(vadj);
      if (val > 0.0 || upper > page)
        return val;
    }
  }
  return pane->tracked_scroll_y;
}

void bt_editorpane_set_scroll_y(BtEditorPane *pane, gdouble scroll_y)
{
  if (pane == NULL)
    return;
  if (scroll_y < 0.0)
    scroll_y = 0.0;
  pane->tracked_scroll_y = scroll_y;
  pane->pending_scroll_y = scroll_y;
  pane->has_pending_scroll = TRUE;
  if (!apply_pending_scroll(pane) && pane->scroll_idle_id == 0 &&
      pane->view != NULL && gtk_widget_get_realized(GTK_WIDGET(pane->view)))
    pane->scroll_idle_id = g_idle_add(on_scroll_idle, pane);
}

gdouble bt_editorpane_viewport_height(const BtEditorPane *pane)
{
  if (pane == NULL)
    return 0.0;
  if (pane->scrolled != NULL) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pane->scrolled));
    if (vadj != NULL) {
      gdouble page = gtk_adjustment_get_page_size(vadj);
      if (page > 1.0)
        return page;
    }
    int h = gtk_widget_get_height(pane->scrolled);
    if (h > 1)
      return (gdouble)h;
  }
  if (pane->view != NULL) {
    GdkRectangle vis = {0};
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(pane->view), &vis);
    if (vis.height > 1)
      return (gdouble)vis.height;
  }
  return 600.0;
}

gboolean bt_editorpane_simulate_click(BtEditorPane *pane, gsize target_byte_offset, guint button,
                                      gint n_press, gdouble drag_dx, gdouble drag_dy,
                                      GdkModifierType mods)
{
  if (pane == NULL || pane->buffer == NULL || pane->relpath == NULL)
    return FALSE;
  BtLinkSpan sp;
  if (!bt_editorpane_link_at_offset(pane, target_byte_offset, &sp))
    return FALSE;

  gsize pre_caret = caret_byte_in_buffer(pane);
  gboolean had_sel = gtk_text_buffer_get_has_selection(GTK_TEXT_BUFFER(pane->buffer));
  gboolean opened = evaluate_and_fire_link_click(pane, button, n_press,
                                                 100.0, 100.0, 100.0 + drag_dx, 100.0 + drag_dy,
                                                 had_sel, mods, pre_caret, &sp);
  if (!opened && button == GDK_BUTTON_PRIMARY) {
    /* Non-activating primary clicks move the caret like normal GtkTextView clicks. */
    place_caret_byte_in_buffer(pane, target_byte_offset);
  }
  return opened;
}

const gchar *bt_editorpane_language_id(const BtEditorPane *pane)
{
  if (pane == NULL || pane->buffer == NULL)
    return "none";
  GtkSourceLanguage *lang = gtk_source_buffer_get_language(pane->buffer);
  return lang ? gtk_source_language_get_id(lang) : "none";
}
