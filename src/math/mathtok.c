/* mathtok.c — GTK-free math delimiter tokenizer (Phase M1, §B.2). */
#include "mathtok.h"

#include <hb-ot.h>
#include <hb.h>
#include <string.h>

#define BT_MATH_MAX_INLINE_BYTES  4096u
#define BT_MATH_MAX_DISPLAY_BYTES 16384u
#define BT_MATH_MAX_DISPLAY_LINES 200u

BtMathScanOpts bt_math_scan_default_opts(void)
{
  BtMathScanOpts o = {
    .allow_paren_bracket = TRUE,
    .allow_environments = TRUE,
    .skip_frontmatter = TRUE,
  };
  return o;
}

const char *bt_math_harfbuzz_version(void)
{
  return hb_version_string();
}

static inline gboolean is_hspace(char c)
{
  return c == ' ' || c == '\t';
}

static inline gboolean is_space_or_eol(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static gboolean is_escaped(const char *s, gsize pos)
{
  gsize bs = 0;
  while (pos > bs && s[pos - 1 - bs] == '\\')
    bs++;
  return (bs & 1u) != 0u;
}

static gsize skip_bom(const char *s, gsize len)
{
  if (len >= 3 && (guchar)s[0] == 0xEFu && (guchar)s[1] == 0xBBu && (guchar)s[2] == 0xBFu)
    return 3;
  return 0;
}

static gsize find_line_end(const char *s, gsize len, gsize pos)
{
  while (pos < len && s[pos] != '\n')
    pos++;
  return pos;
}

static gboolean line_is_blank(const char *s, gsize ls, gsize le)
{
  for (gsize i = ls; i < le; i++) {
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
      return FALSE;
  }
  return TRUE;
}

/* Skips optional blockquote prefix '>' and returns offset of first content byte on line. */
static gsize skip_bq_and_indent(const char *s, gsize ls, gsize le, guint *out_bq, guint *out_spaces)
{
  gsize p = ls;
  guint bq = 0;
  for (;;) {
    gsize q = p;
    guint sp = 0;
    while (q < le && sp < 3 && s[q] == ' ') {
      sp++;
      q++;
    }
    if (q < le && s[q] == '>') {
      bq++;
      q++;
      if (q < le && s[q] == ' ')
        q++;
      p = q;
      continue;
    }
    break;
  }
  guint indent = 0;
  while (p < le && (s[p] == ' ' || s[p] == '\t')) {
    indent += (s[p] == '\t') ? 4u : 1u;
    p++;
  }
  if (out_bq) *out_bq = bq;
  if (out_spaces) *out_spaces = indent;
  return p;
}

/* Detects YAML frontmatter bounds at top of doc; sets *out_end past closing --- line. */
static gboolean match_frontmatter(const char *s, gsize len, gsize start_pos,
                                  gsize *out_end, guint *out_lines)
{
  gsize le0 = find_line_end(s, len, start_pos);
  gsize trimmed_end = le0;
  while (trimmed_end > start_pos &&
         (s[trimmed_end - 1] == ' ' || s[trimmed_end - 1] == '\t' || s[trimmed_end - 1] == '\r'))
    trimmed_end--;
  if (trimmed_end - start_pos != 3 || memcmp(s + start_pos, "---", 3) != 0)
    return FALSE;
  if (le0 >= len)
    return FALSE;

  gsize pos = le0 + 1;
  guint lines = 1;
  while (pos < len) {
    gsize le = find_line_end(s, len, pos);
    lines++;
    gsize te = le;
    while (te > pos && (s[te - 1] == ' ' || s[te - 1] == '\t' || s[te - 1] == '\r'))
      te--;
    if (te - pos == 3 && memcmp(s + pos, "---", 3) == 0) {
      *out_end = (le < len) ? (le + 1) : len;
      *out_lines = lines;
      return TRUE;
    }
    pos = (le < len) ? (le + 1) : len;
  }
  return FALSE;
}

static gboolean parse_fence_open(const char *s, gsize content_start, gsize le,
                                 char *out_ch, guint *out_len)
{
  if (content_start >= le)
    return FALSE;
  char ch = s[content_start];
  if (ch != '`' && ch != '~')
    return FALSE;
  gsize p = content_start;
  while (p < le && s[p] == ch)
    p++;
  guint flen = (guint)(p - content_start);
  if (flen < 3)
    return FALSE;
  if (ch == '`') {
    for (gsize q = p; q < le; q++) {
      if (s[q] == '`')
        return FALSE;
    }
  }
  *out_ch = ch;
  *out_len = flen;
  return TRUE;
}

static gboolean parse_fence_close(const char *s, gsize content_start, gsize le,
                                  char want_ch, guint min_len)
{
  if (content_start >= le || s[content_start] != want_ch)
    return FALSE;
  gsize p = content_start;
  while (p < le && s[p] == want_ch)
    p++;
  if ((guint)(p - content_start) < min_len)
    return FALSE;
  while (p < le) {
    if (s[p] != ' ' && s[p] != '\t' && s[p] != '\r')
      return FALSE;
    p++;
  }
  return TRUE;
}

static gboolean starts_list_marker(const char *s, gsize content_start, gsize le)
{
  if (content_start + 1 < le &&
      (s[content_start] == '-' || s[content_start] == '*' || s[content_start] == '+') &&
      is_hspace(s[content_start + 1]))
    return TRUE;
  gsize p = content_start;
  guint digits = 0;
  while (p < le && g_ascii_isdigit(s[p]) && digits < 9) {
    digits++;
    p++;
  }
  if (digits > 0 && p + 1 < le && (s[p] == '.' || s[p] == ')') && is_hspace(s[p + 1]))
    return TRUE;
  return FALSE;
}

static guint32 hash_normalized_math(const char *s, gsize src_start, gsize src_end, BtMathMode mode)
{
  guint32 h = 2166136261u;
  h ^= (guint32)mode;
  h *= 16777619u;

  while (src_start < src_end && is_space_or_eol(s[src_start]))
    src_start++;
  while (src_end > src_start && is_space_or_eol(s[src_end - 1]))
    src_end--;

  gboolean at_line_start = FALSE;
  for (gsize i = src_start; i < src_end; i++) {
    char c = s[i];
    if (c == '\r')
      continue;
    if (at_line_start && mode == BT_MATH_DISPLAY) {
      gsize q = i;
      while (q < src_end && is_hspace(s[q]))
        q++;
      if (q < src_end && s[q] == '>') {
        q++;
        if (q < src_end && s[q] == ' ')
          q++;
        i = q;
        at_line_start = FALSE;
        if (i >= src_end)
          break;
        c = s[i];
        if (c == '\r')
          continue;
      } else {
        at_line_start = FALSE;
      }
    }
    h ^= (guint32)(guchar)c;
    h *= 16777619u;
    if (c == '\n')
      at_line_start = TRUE;
  }
  return h;
}

static const char *const MATH_ENVS[] = {
  "equation*", "equation", "align*", "align", "gather*", "gather", "aligned", NULL
};

static gboolean match_begin_env(const char *s, gsize len, gsize pos,
                                char *out_name, gsize out_sz, gsize *out_after)
{
  static const char prefix[] = "\\begin{";
  gsize plen = sizeof(prefix) - 1;
  if (pos + plen >= len || memcmp(s + pos, prefix, plen) != 0)
    return FALSE;
  gsize name_start = pos + plen;
  gsize p = name_start;
  while (p < len && s[p] != '}' && p - name_start < 32 && s[p] != '\n' && s[p] != '\r')
    p++;
  if (p >= len || s[p] != '}')
    return FALSE;
  gsize nlen = p - name_start;
  if (nlen == 0 || nlen >= out_sz)
    return FALSE;
  gboolean allowed = FALSE;
  for (guint k = 0; MATH_ENVS[k] != NULL; k++) {
    if (strlen(MATH_ENVS[k]) == nlen && memcmp(s + name_start, MATH_ENVS[k], nlen) == 0) {
      allowed = TRUE;
      break;
    }
  }
  if (!allowed)
    return FALSE;
  memcpy(out_name, s + name_start, nlen);
  out_name[nlen] = '\0';
  *out_after = p + 1;
  return TRUE;
}

/* Scans a contiguous slice starting at slice_pos (outside frontmatter/code fences). */
static void scan_internal(const char *s, gsize len, gsize limit_end, const BtMathScanOpts *opts,
                          gsize start_pos, guint start_line, GArray *out)
{
  gsize pos = start_pos;
  guint line = start_line;
  gboolean at_line_start = TRUE;
  gboolean prev_line_blank = TRUE;
  gboolean in_fence = FALSE;
  char fence_ch = 0;
  guint fence_len = 0;

  while (pos < limit_end) {
    if (at_line_start) {
      gsize le = find_line_end(s, len, pos);
      if (line_is_blank(s, pos, le)) {
        prev_line_blank = TRUE;
        pos = (le < limit_end) ? (le + 1) : limit_end;
        line++;
        at_line_start = TRUE;
        continue;
      }

      guint bq = 0, spaces = 0;
      gsize cs = skip_bq_and_indent(s, pos, le, &bq, &spaces);

      if (in_fence) {
        if (spaces <= 3 && parse_fence_close(s, cs, le, fence_ch, fence_len))
          in_fence = FALSE;
        pos = (le < limit_end) ? (le + 1) : limit_end;
        line++;
        prev_line_blank = FALSE;
        at_line_start = TRUE;
        continue;
      }

      /* Indented code block (>=4 leading spaces after a blank line, not a nested list). */
      if (prev_line_blank && bq == 0 && spaces >= 4 && !starts_list_marker(s, cs, le)) {
        pos = (le < limit_end) ? (le + 1) : limit_end;
        line++;
        /* Stay in_indented-like blank behavior: keep skipping contiguous >=4-space lines. */
        while (pos < limit_end) {
          gsize nle = find_line_end(s, len, pos);
          if (line_is_blank(s, pos, nle))
            break;
          guint nbq = 0, nsp = 0;
          skip_bq_and_indent(s, pos, nle, &nbq, &nsp);
          if (nbq != 0 || nsp < 4)
            break;
          pos = (nle < limit_end) ? (nle + 1) : limit_end;
          line++;
        }
        prev_line_blank = FALSE;
        at_line_start = TRUE;
        continue;
      }

      char fch = 0;
      guint flen = 0;
      if (spaces <= 3 && parse_fence_open(s, cs, le, &fch, &flen)) {
        in_fence = TRUE;
        fence_ch = fch;
        fence_len = flen;
        pos = (le < limit_end) ? (le + 1) : limit_end;
        line++;
        prev_line_blank = FALSE;
        at_line_start = TRUE;
        continue;
      }

      prev_line_blank = FALSE;
      at_line_start = FALSE;
    }

    char c = s[pos];
    if (c == '\n') {
      line++;
      pos++;
      at_line_start = TRUE;
      continue;
    }

    /* 1. Inline code spans: exact N backtick match within same paragraph. */
    if (c == '`') {
      gsize run = 0;
      while (pos + run < len && s[pos + run] == '`')
        run++;
      gsize q = pos + run;
      gboolean closed = FALSE;
      while (q < len) {
        if (s[q] == '\n') {
          gsize nls = q + 1;
          gsize nle = find_line_end(s, len, nls);
          if (line_is_blank(s, nls, nle))
            break;
          q++;
          continue;
        }
        if (s[q] == '`') {
          gsize crun = 0;
          while (q + crun < len && s[q + crun] == '`')
            crun++;
          if (crun == run) {
            for (gsize k = pos; k < q + crun; k++) {
              if (s[k] == '\n')
                line++;
            }
            pos = q + crun;
            closed = TRUE;
            break;
          }
          q += crun;
          continue;
        }
        q++;
      }
      if (!closed)
        pos += run;
      continue;
    }

    /* 2. HTML comments <!-- ... --> */
    if (c == '<' && pos + 4 <= len && memcmp(s + pos, "<!--", 4) == 0) {
      gsize q = pos + 4;
      gboolean found = FALSE;
      while (q + 3 <= len) {
        if (memcmp(s + q, "-->", 3) == 0) {
          for (gsize k = pos; k < q + 3; k++) {
            if (s[k] == '\n')
              line++;
          }
          pos = q + 3;
          found = TRUE;
          break;
        }
        q++;
      }
      if (!found)
        pos += 4;
      continue;
    }

    /* 3. Wikilink [[...]] takes everything inside [[ ]] out of math scanning. */
    if (c == '[' && pos + 1 < len && s[pos + 1] == '[' && !is_escaped(s, pos)) {
      gsize q = pos + 2;
      gboolean ok = FALSE;
      while (q + 1 < len && s[q] != '\n' && s[q] != '\r' && q - (pos + 2) <= 1024) {
        if (s[q] == '[')
          break;
        if (s[q] == ']' && s[q + 1] == ']') {
          ok = TRUE;
          pos = q + 2;
          break;
        }
        q++;
      }
      if (!ok)
        pos += 2;
      continue;
    }

    /* 4. Markdown link destination ](...) (only when we see ']' '(' at top level):
     * Link text [...] may hold inline math, while destination (...) is skipped. */
    if (c == ']' && pos + 1 < len && s[pos + 1] == '(' && !is_escaped(s, pos)) {
      gsize q = pos + 2;
      while (q < len && is_hspace(s[q]))
        q++;
      if (q < len && s[q] == '<') {
        q++;
        while (q < len && s[q] != '>' && s[q] != '\n' && s[q] != '\r') {
          if (s[q] == '\\' && q + 1 < len)
            q++;
          q++;
        }
        if (q < len && s[q] == '>')
          q++;
        while (q < len && s[q] != ')' && s[q] != '\n' && s[q] != '\r')
          q++;
        if (q < len && s[q] == ')') {
          pos = q + 1;
          continue;
        }
      } else {
        int depth = 1;
        while (q < len && s[q] != '\n' && s[q] != '\r') {
          if (s[q] == '\\' && q + 1 < len) {
            q += 2;
            continue;
          }
          if (s[q] == '(')
            depth++;
          else if (s[q] == ')') {
            depth--;
            if (depth == 0) {
              pos = q + 1;
              break;
            }
          }
          q++;
        }
        if (depth == 0)
          continue;
      }
      pos += 2;
      continue;
    }

    /* 5. Bare URLs http:// or https:// */
    if (c == 'h' && !is_escaped(s, pos) &&
        ((pos + 7 <= len && memcmp(s + pos, "http://", 7) == 0) ||
         (pos + 8 <= len && memcmp(s + pos, "https://", 8) == 0)) &&
        (pos == 0 || is_space_or_eol(s[pos - 1]) || s[pos - 1] == '(' || s[pos - 1] == '<')) {
      gsize q = pos + 7;
      while (q < len && !is_space_or_eol(s[q]) && s[q] != '<' && s[q] != '>')
        q++;
      pos = q;
      continue;
    }

    /* 6. Display math $$...$$ */
    if (c == '$' && !is_escaped(s, pos) && pos + 1 < len && s[pos + 1] == '$') {
      gsize src_start = pos + 2;
      gsize q = src_start;
      guint end_line = line;
      gboolean found = FALSE;
      gboolean fenced = FALSE;

      while (q + 1 < len && (q - src_start) <= BT_MATH_MAX_DISPLAY_BYTES) {
        if (s[q] == '\n') {
          end_line++;
          if ((end_line - line + 1u) > BT_MATH_MAX_DISPLAY_LINES)
            break;
          gsize le = find_line_end(s, len, q + 1);
          guint bq = 0, sp = 0;
          gsize cs = skip_bq_and_indent(s, q + 1, le, &bq, &sp);
          char fch = 0;
          guint flen = 0;
          if (sp <= 3 && parse_fence_open(s, cs, le, &fch, &flen)) {
            fenced = TRUE;
            break;
          }
          q++;
          continue;
        }
        if (s[q] == '$' && s[q + 1] == '$' && !is_escaped(s, q)) {
          found = TRUE;
          break;
        }
        q++;
      }

      if (found && !fenced && (q - src_start) <= BT_MATH_MAX_DISPLAY_BYTES &&
          (end_line - line + 1u) <= BT_MATH_MAX_DISPLAY_LINES) {
        BtMathSpan sp = {
          .start = pos,
          .end = q + 2,
          .src_start = src_start,
          .src_end = q,
          .mode = BT_MATH_DISPLAY,
          .hash = hash_normalized_math(s, src_start, q, BT_MATH_DISPLAY),
          .line_first = line,
          .line_last = end_line,
        };
        g_array_append_val(out, sp);
        line = end_line;
        pos = q + 2;
        continue;
      }
      pos += 2;
      continue;
    }

    /* 7. Inline math $...$ (Pandoc rules B.2.2) */
    if (c == '$' && !is_escaped(s, pos) && (pos + 1 >= len || s[pos + 1] != '$')) {
      if (pos + 1 < len && !is_space_or_eol(s[pos + 1])) {
        gsize src_start = pos + 1;
        gsize q = src_start;
        guint end_line = line;
        gboolean closed = FALSE;

        while (q < len && (q - src_start) <= BT_MATH_MAX_INLINE_BYTES) {
          if (s[q] == '\n') {
            end_line++;
            gsize nls = q + 1;
            gsize nle = find_line_end(s, len, nls);
            if (line_is_blank(s, nls, nle))
              break;
            guint bq = 0, sp = 0;
            gsize cs = skip_bq_and_indent(s, nls, nle, &bq, &sp);
            char fch = 0;
            guint flen = 0;
            if (sp <= 3 && parse_fence_open(s, cs, nle, &fch, &flen))
              break;
            q++;
            continue;
          }
          if (s[q] == '$' && !is_escaped(s, q)) {
            if (q + 1 < len && s[q + 1] == '$')
              break; /* Don't steal half of a $$ display delimiter. */
            gboolean pre_ok = (q > src_start) && !is_space_or_eol(s[q - 1]);
            gboolean post_ok = (q + 1 >= len) || !g_ascii_isdigit(s[q + 1]);
            if (pre_ok && post_ok)
              closed = TRUE;
            break;
          }
          q++;
        }

        if (closed) {
          BtMathSpan sp = {
            .start = pos,
            .end = q + 1,
            .src_start = src_start,
            .src_end = q,
            .mode = BT_MATH_INLINE,
            .hash = hash_normalized_math(s, src_start, q, BT_MATH_INLINE),
            .line_first = line,
            .line_last = end_line,
          };
          g_array_append_val(out, sp);
          line = end_line;
          pos = q + 1;
          continue;
        }
      }
      pos++;
      continue;
    }

    /* 8. Optional \(...\) and \[...\], plus top-level \begin{env}...\end{env}. */
    if (c == '\\' && !is_escaped(s, pos) && pos + 1 < len) {
      if (opts->allow_paren_bracket && s[pos + 1] == '(') {
        gsize src_start = pos + 2;
        gsize q = src_start;
        guint end_line = line;
        gboolean closed = FALSE;
        while (q + 1 < len && (q - src_start) <= BT_MATH_MAX_INLINE_BYTES) {
          if (s[q] == '\n') {
            end_line++;
            gsize nle = find_line_end(s, len, q + 1);
            if (line_is_blank(s, q + 1, nle))
              break;
            q++;
            continue;
          }
          if (s[q] == '\\' && s[q + 1] == ')' && !is_escaped(s, q)) {
            closed = TRUE;
            break;
          }
          q++;
        }
        if (closed) {
          BtMathSpan sp = {
            .start = pos, .end = q + 2, .src_start = src_start, .src_end = q,
            .mode = BT_MATH_INLINE,
            .hash = hash_normalized_math(s, src_start, q, BT_MATH_INLINE),
            .line_first = line, .line_last = end_line,
          };
          g_array_append_val(out, sp);
          line = end_line;
          pos = q + 2;
          continue;
        }
      } else if (opts->allow_paren_bracket && s[pos + 1] == '[') {
        gsize src_start = pos + 2;
        gsize q = src_start;
        guint end_line = line;
        gboolean closed = FALSE;
        while (q + 1 < len && (q - src_start) <= BT_MATH_MAX_DISPLAY_BYTES) {
          if (s[q] == '\n') {
            end_line++;
            if ((end_line - line + 1u) > BT_MATH_MAX_DISPLAY_LINES)
              break;
          }
          if (s[q] == '\\' && s[q + 1] == ']' && !is_escaped(s, q)) {
            closed = TRUE;
            break;
          }
          q++;
        }
        if (closed && (end_line - line + 1u) <= BT_MATH_MAX_DISPLAY_LINES) {
          BtMathSpan sp = {
            .start = pos, .end = q + 2, .src_start = src_start, .src_end = q,
            .mode = BT_MATH_DISPLAY,
            .hash = hash_normalized_math(s, src_start, q, BT_MATH_DISPLAY),
            .line_first = line, .line_last = end_line,
          };
          g_array_append_val(out, sp);
          line = end_line;
          pos = q + 2;
          continue;
        }
      } else if (opts->allow_environments) {
        char env_name[32];
        gsize after_begin = 0;
        if (match_begin_env(s, len, pos, env_name, sizeof(env_name), &after_begin)) {
          gchar *end_pat = g_strdup_printf("\\end{%s}", env_name);
          gsize epat_len = strlen(end_pat);
          gsize q = after_begin;
          guint end_line = line;
          gboolean closed = FALSE;
          while (q + epat_len <= len && (q - pos) <= BT_MATH_MAX_DISPLAY_BYTES) {
            if (s[q] == '\n') {
              end_line++;
              if ((end_line - line + 1u) > BT_MATH_MAX_DISPLAY_LINES)
                break;
            }
            if (s[q] == '\\' && memcmp(s + q, end_pat, epat_len) == 0 && !is_escaped(s, q)) {
              closed = TRUE;
              break;
            }
            q++;
          }
          if (closed && (end_line - line + 1u) <= BT_MATH_MAX_DISPLAY_LINES) {
            BtMathSpan sp = {
              .start = pos, .end = q + epat_len, .src_start = pos, .src_end = q + epat_len,
              .mode = BT_MATH_DISPLAY,
              .hash = hash_normalized_math(s, pos, q + epat_len, BT_MATH_DISPLAY),
              .line_first = line, .line_last = end_line,
            };
            g_array_append_val(out, sp);
            g_free(end_pat);
            line = end_line;
            pos = q + epat_len;
            continue;
          }
          g_free(end_pat);
        }
      }
    }

    pos++;
  }
}

GArray *bt_math_scan(const char *text, gsize len, const BtMathScanOpts *opts)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(BtMathSpan));
  if (text == NULL || len == 0)
    return out;

  BtMathScanOpts local = opts ? *opts : bt_math_scan_default_opts();
  gsize pos = skip_bom(text, len);
  guint line = 1;

  if (local.skip_frontmatter && pos < len) {
    gsize fm_end = pos;
    guint fm_lines = 0;
    if (match_frontmatter(text, len, pos, &fm_end, &fm_lines)) {
      pos = fm_end;
      line += fm_lines;
    }
  }

  scan_internal(text, len, len, &local, pos, line, out);
  return out;
}

GArray *bt_math_scan_range(const char *text, gsize len, gsize range_start, gsize range_end,
                           const BtMathScanOpts *opts,
                           gsize *out_block_start, gsize *out_block_end)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(BtMathSpan));
  if (text == NULL || len == 0) {
    if (out_block_start) *out_block_start = 0;
    if (out_block_end) *out_block_end = 0;
    return out;
  }

  if (range_start > len) range_start = len;
  if (range_end > len) range_end = len;
  if (range_end < range_start) range_end = range_start;

  BtMathScanOpts local = opts ? *opts : bt_math_scan_default_opts();
  gsize bom = skip_bom(text, len);
  gsize body_start = bom;
  guint body_line = 1;
  if (local.skip_frontmatter && bom < len) {
    gsize fm_end = bom;
    guint fm_lines = 0;
    if (match_frontmatter(text, len, bom, &fm_end, &fm_lines)) {
      body_start = fm_end;
      body_line += fm_lines;
    }
  }

  /* Walk lines from body_start, partitioning into paragraph/block chunks separated
   * by blank lines outside fences / multi-line display blocks / comments. Any chunk
   * overlapping [range_start, range_end] becomes the minimal self-contained
   * rescan window [win_start, win_end), guaranteeing exact equivalence with full scan. */
  if (range_end <= body_start) {
    if (out_block_start) *out_block_start = 0;
    if (out_block_end) *out_block_end = body_start;
    return out;
  }

  gsize win_start = len;
  gsize win_end = len;
  guint win_line = body_line;

  gsize pos = body_start;
  guint line = body_line;
  gboolean in_fence = FALSE;
  char fence_ch = 0;
  guint fence_len = 0;
  gboolean in_display = FALSE;
  gsize chunk_start = body_start;
  guint chunk_line = body_line;

  while (pos < len) {
    gsize le = find_line_end(text, len, pos);
    guint bq = 0, spaces = 0;
    gsize cs = skip_bq_and_indent(text, pos, le, &bq, &spaces);
    gboolean blank = line_is_blank(text, pos, le);

    if (in_fence) {
      if (spaces <= 3 && parse_fence_close(text, cs, le, fence_ch, fence_len))
        in_fence = FALSE;
    } else if (!blank) {
      char fch = 0;
      guint flen = 0;
      if (!in_display && spaces <= 3 && parse_fence_open(text, cs, le, &fch, &flen)) {
        in_fence = TRUE;
        fence_ch = fch;
        fence_len = flen;
      } else {
        for (gsize i = pos; i + 1 < le; i++) {
          if (text[i] == '$' && text[i + 1] == '$' && !is_escaped(text, i)) {
            in_display = !in_display;
            i++;
          }
        }
      }
    } else if (!in_display) {
      gsize c_end = (le < len) ? (le + 1) : len;
      if (win_start == len && c_end >= range_start) {
        win_start = chunk_start;
        win_line = chunk_line;
      }
      if (win_start != len && pos >= range_end) {
        win_end = pos;
        break;
      }
      chunk_start = c_end;
      chunk_line = line + 1;
    }

    pos = (le < len) ? (le + 1) : len;
    line++;
  }

  if (win_start == len) {
    win_start = chunk_start;
    win_line = chunk_line;
  }
  if (win_end > len)
    win_end = len;
  if (win_start > body_start && win_start == len)
    win_start = body_start;

  scan_internal(text, win_end, win_end, &local, win_start, win_line, out);
  if (out_block_start) *out_block_start = win_start;
  if (out_block_end) *out_block_end = win_end;
  return out;
}

gboolean bt_math_offset_in_spans(const GArray *spans, gsize byte_offset, BtMathSpan *out_span)
{
  if (spans == NULL || spans->len == 0)
    return FALSE;
  guint lo = 0, hi = spans->len;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    const BtMathSpan *sp = &g_array_index(spans, BtMathSpan, mid);
    if (byte_offset < sp->start)
      hi = mid;
    else if (byte_offset >= sp->end)
      lo = mid + 1;
    else {
      if (out_span != NULL)
        *out_span = *sp;
      return TRUE;
    }
  }
  return FALSE;
}
