/* mdparse.c — MD4C-based extractor for headings, wiki-links, #tags, plain text. */
#include "mdparse.h"

#include <md4c.h>
#include <string.h>

typedef struct {
  BtMdExtract *ex;
  GHashTable *seen_links;
  GHashTable *seen_tags;
  GString *plain;
  GString *h1_buf;
  GString *prose_buf; /* current non-code text span for [[link]]/#tag scanning */
  gboolean in_h1;
  gint code_depth;
} ParseCtx;

static void add_unique(GPtrArray *arr, GHashTable *seen, gchar *val)
{
  if (val == NULL || val[0] == '\0') {
    g_free(val);
    return;
  }
  if (g_hash_table_contains(seen, val)) {
    g_free(val);
    return;
  }
  g_hash_table_add(seen, g_strdup(val));
  g_ptr_array_add(arr, val);
}

static void scan_prose_for_links_and_tags(ParseCtx *ctx, const gchar *s, gsize len)
{
  /* 1. Extract [[wiki-links]] */
  const gchar *p = s;
  const gchar *end = s + len;
  while (p < end) {
    const gchar *open = strstr(p, "[[");
    if (open == NULL || open >= end)
      break;
    const gchar *close = strstr(open + 2, "]]");
    if (close == NULL || close >= end)
      break;
    gchar *inner = g_strndup(open + 2, (gsize)(close - (open + 2)));
    gchar *cut = strpbrk(inner, "|#");
    if (cut != NULL)
      *cut = '\0';
    g_strstrip(inner);
    if (inner[0] != '\0' && strstr(inner, "[[") == NULL && strchr(inner, '\n') == NULL)
      add_unique(ctx->ex->wikilinks, ctx->seen_links, inner);
    else
      g_free(inner);
    p = close + 2;
  }

  /* 2. Extract inline #tags (must be at start or preceded by whitespace). */
  for (gsize i = 0; i < len; i++) {
    if (s[i] != '#')
      continue;
    if (i > 0 && !g_ascii_isspace(s[i - 1]))
      continue;
    if (i + 1 >= len || !g_ascii_isalpha(s[i + 1]))
      continue;
    gsize j = i + 1;
    while (j < len && (g_ascii_isalnum(s[j]) || s[j] == '_' || s[j] == '-' || s[j] == '/'))
      j++;
    gchar *tag = g_ascii_strdown(s + i + 1, (gssize)(j - (i + 1)));
    add_unique(ctx->ex->tags, ctx->seen_tags, tag);
    i = j - 1;
  }
}

static void flush_prose(ParseCtx *ctx)
{
  if (ctx->prose_buf->len > 0) {
    scan_prose_for_links_and_tags(ctx, ctx->prose_buf->str, ctx->prose_buf->len);
    g_string_truncate(ctx->prose_buf, 0);
  }
}

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *ud)
{
  ParseCtx *ctx = ud;
  flush_prose(ctx);
  if (type == MD_BLOCK_CODE) {
    ctx->code_depth++;
  } else if (type == MD_BLOCK_H) {
    const MD_BLOCK_H_DETAIL *hd = detail;
    if (hd->level == 1 && ctx->ex->first_h1 == NULL) {
      ctx->in_h1 = TRUE;
      g_string_truncate(ctx->h1_buf, 0);
    }
  }
  return 0;
}

static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *ud)
{
  (void)detail;
  ParseCtx *ctx = ud;
  flush_prose(ctx);
  if (type == MD_BLOCK_CODE) {
    if (ctx->code_depth > 0)
      ctx->code_depth--;
  } else if (type == MD_BLOCK_H && ctx->in_h1) {
    ctx->in_h1 = FALSE;
    if (ctx->h1_buf->len > 0 && ctx->ex->first_h1 == NULL) {
      gchar *h = g_strdup(ctx->h1_buf->str);
      g_strstrip(h);
      if (h[0] != '\0')
        ctx->ex->first_h1 = h;
      else
        g_free(h);
    }
  }
  if (ctx->plain->len > 0 && ctx->plain->str[ctx->plain->len - 1] != '\n')
    g_string_append_c(ctx->plain, '\n');
  return 0;
}

static int cb_enter_span(MD_SPANTYPE type, void *detail, void *ud)
{
  (void)detail;
  ParseCtx *ctx = ud;
  if (type == MD_SPAN_CODE) {
    flush_prose(ctx);
    ctx->code_depth++;
  }
  return 0;
}

static int cb_leave_span(MD_SPANTYPE type, void *detail, void *ud)
{
  (void)detail;
  ParseCtx *ctx = ud;
  if (type == MD_SPAN_CODE && ctx->code_depth > 0)
    ctx->code_depth--;
  return 0;
}

static int cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud)
{
  ParseCtx *ctx = ud;
  if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
    flush_prose(ctx);
    g_string_append_c(ctx->plain, ' ');
    return 0;
  }
  g_string_append_len(ctx->plain, text, (gssize)size);
  if (ctx->in_h1)
    g_string_append_len(ctx->h1_buf, text, (gssize)size);
  if (ctx->code_depth == 0 && type != MD_TEXT_CODE)
    g_string_append_len(ctx->prose_buf, text, (gssize)size);
  return 0;
}

BtMdExtract *bt_mdparse_extract(const gchar *markdown, gsize len)
{
  BtMdExtract *ex = g_new0(BtMdExtract, 1);
  ex->wikilinks = g_ptr_array_new_with_free_func(g_free);
  ex->tags = g_ptr_array_new_with_free_func(g_free);

  if (markdown == NULL || len == 0) {
    ex->plain_body = g_strdup("");
    return ex;
  }

  ParseCtx ctx = {
    .ex = ex,
    .seen_links = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    .seen_tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    .plain = g_string_sized_new(len),
    .h1_buf = g_string_new(NULL),
    .prose_buf = g_string_sized_new(256),
  };

  MD_PARSER parser;
  memset(&parser, 0, sizeof(parser));
  parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS;
  parser.enter_block = cb_enter_block;
  parser.leave_block = cb_leave_block;
  parser.enter_span = cb_enter_span;
  parser.leave_span = cb_leave_span;
  parser.text = cb_text;

  md_parse(markdown, (MD_SIZE)len, &parser, &ctx);
  flush_prose(&ctx);

  ex->plain_body = g_string_free(ctx.plain, FALSE);
  g_string_free(ctx.h1_buf, TRUE);
  g_string_free(ctx.prose_buf, TRUE);
  g_hash_table_destroy(ctx.seen_links);
  g_hash_table_destroy(ctx.seen_tags);
  return ex;
}

void bt_mdparse_free(BtMdExtract *ex)
{
  if (ex == NULL)
    return;
  g_free(ex->first_h1);
  g_ptr_array_unref(ex->wikilinks);
  g_ptr_array_unref(ex->tags);
  g_free(ex->plain_body);
  g_free(ex);
}
