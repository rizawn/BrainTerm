/* linkresolve.c — GTK-free clickable link scanner, resolver, and A.9 security policy (Phase L1). */
#include "linkresolve.h"
#include "frontmatter.h"
#include "math/mathtok.h"
#include "platform.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

#define BT_LINK_MAX_TARGET_BYTES 1024u
#define BT_LINK_MAX_URL_BYTES    2048u

/* ----------------------------------------------------- small char helpers -- */

static inline gboolean is_hspace(char c)
{
  return c == ' ' || c == '\t';
}

static inline gboolean is_eol_or_space(char c)
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

static gboolean is_note_ext(const char *path)
{
  if (path == NULL)
    return FALSE;
  gsize n = strlen(path);
  return (n > 3 && g_ascii_strcasecmp(path + n - 3, ".md") == 0) ||
         (n > 9 && g_ascii_strcasecmp(path + n - 9, ".markdown") == 0);
}

static gchar *strip_note_ext(const char *path)
{
  if (path == NULL)
    return NULL;
  gsize n = strlen(path);
  if (n > 3 && g_ascii_strcasecmp(path + n - 3, ".md") == 0)
    return g_strndup(path, n - 3);
  if (n > 9 && g_ascii_strcasecmp(path + n - 9, ".markdown") == 0)
    return g_strndup(path, n - 9);
  return g_strdup(path);
}

static const char *path_basename_ptr(const char *relpath)
{
  const char *slash = strrchr(relpath, '/');
  return slash ? (slash + 1) : relpath;
}

static gchar *path_dirname_dup(const char *relpath)
{
  if (relpath == NULL || relpath[0] == '\0')
    return g_strdup("");
  const char *slash = strrchr(relpath, '/');
  if (slash == NULL)
    return g_strdup("");
  return g_strndup(relpath, (gsize)(slash - relpath));
}

static gchar *normalize_key(const char *s, gboolean case_sensitive)
{
  if (s == NULL)
    return g_strdup("");
  g_autofree gchar *nfc = g_utf8_normalize(s, -1, G_NORMALIZE_NFC);
  const gchar *src = nfc ? nfc : s;
  if (case_sensitive)
    return g_strdup(src);
  return g_utf8_casefold(src, -1);
}

/* ---------------------------------------------------------- span lifecycle -- */

static void clear_link_span(gpointer data)
{
  BtLinkSpan *sp = data;
  g_free(sp->raw_target);
  g_free(sp->md_label);
}

static GArray *new_span_array(void)
{
  GArray *arr = g_array_new(FALSE, FALSE, sizeof(BtLinkSpan));
  g_array_set_clear_func(arr, clear_link_span);
  return arr;
}

/* ------------------------------------------------------ scanner helpers --- */

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

static gsize skip_bq_and_indent(const char *s, gsize ls, gsize le, guint *out_bq, guint *out_sp)
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
  if (out_sp) *out_sp = indent;
  return p;
}

static gboolean starts_list_marker(const char *s, gsize cs, gsize le)
{
  if (cs + 1 < le && (s[cs] == '-' || s[cs] == '*' || s[cs] == '+') && is_hspace(s[cs + 1]))
    return TRUE;
  gsize p = cs;
  guint digits = 0;
  while (p < le && g_ascii_isdigit(s[p]) && digits < 9) {
    digits++;
    p++;
  }
  return (digits > 0 && p + 1 < le && (s[p] == '.' || s[p] == ')') && is_hspace(s[p + 1]));
}

static gboolean parse_fence_open(const char *s, gsize cs, gsize le, char *out_ch, guint *out_len)
{
  if (cs >= le)
    return FALSE;
  char ch = s[cs];
  if (ch != '`' && ch != '~')
    return FALSE;
  gsize p = cs;
  while (p < le && s[p] == ch)
    p++;
  guint flen = (guint)(p - cs);
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

static gboolean parse_fence_close(const char *s, gsize cs, gsize le, char want_ch, guint min_len)
{
  if (cs >= le || s[cs] != want_ch)
    return FALSE;
  gsize p = cs;
  while (p < le && s[p] == want_ch)
    p++;
  if ((guint)(p - cs) < min_len)
    return FALSE;
  while (p < le) {
    if (s[p] != ' ' && s[p] != '\t' && s[p] != '\r')
      return FALSE;
    p++;
  }
  return TRUE;
}

static gboolean match_frontmatter_bounds(const char *s, gsize len, gsize start_pos,
                                         gsize *out_body_off, gsize *out_close_line_start,
                                         guint *out_lines)
{
  gsize le0 = find_line_end(s, len, start_pos);
  gsize te0 = le0;
  while (te0 > start_pos && is_eol_or_space(s[te0 - 1]))
    te0--;
  if (te0 - start_pos != 3 || memcmp(s + start_pos, "---", 3) != 0 || le0 >= len)
    return FALSE;

  gsize pos = le0 + 1;
  guint lines = 1;
  while (pos < len) {
    gsize le = find_line_end(s, len, pos);
    lines++;
    gsize te = le;
    while (te > pos && is_eol_or_space(s[te - 1]))
      te--;
    if (te - pos == 3 && memcmp(s + pos, "---", 3) == 0) {
      *out_close_line_start = pos;
      *out_body_off = (le < len) ? (le + 1) : len;
      *out_lines = lines;
      return TRUE;
    }
    pos = (le < len) ? (le + 1) : len;
  }
  return FALSE;
}

static void scan_frontmatter_links(const char *s, gsize fm_start, gsize fm_close_line_start,
                                   GArray *out)
{
  gsize pos = fm_start;
  guint line = 1;
  gsize le0 = find_line_end(s, fm_close_line_start, pos);
  pos = (le0 < fm_close_line_start) ? (le0 + 1) : fm_close_line_start;
  line++;

  gboolean in_links_block = FALSE;
  while (pos < fm_close_line_start) {
    gsize le = find_line_end(s, fm_close_line_start, pos);
    gsize ls = pos;
    while (ls < le && s[ls] == ' ')
      ls++;

    if (pos == ls && le - ls >= 6 && memcmp(s + ls, "links:", 6) == 0) {
      in_links_block = TRUE;
    } else if (pos == ls && ls < le && s[ls] != '-' && s[ls] != '#') {
      in_links_block = FALSE;
    }

    if (in_links_block) {
      for (gsize p = pos; p + 3 < le; p++) {
        if (s[p] == '[' && s[p + 1] == '[' && !is_escaped(s, p)) {
          gsize q = p + 2;
          gboolean valid = FALSE;
          while (q + 1 < le && q - (p + 2) <= BT_LINK_MAX_TARGET_BYTES) {
            if (s[q] == '[')
              break;
            if (s[q] == ']' && s[q + 1] == ']') {
              valid = TRUE;
              break;
            }
            if (s[q] == ']')
              break;
            q++;
          }
          if (valid && q > p + 2) {
            BtLinkSpan sp = {
              .start = p,
              .end = q + 2,
              .target_start = p + 2,
              .target_end = q,
              .line_no = line,
              .syntax = BT_LINK_SYNTAX_FRONTMATTER,
              .raw_target = g_strndup(s + p + 2, q - (p + 2)),
              .md_label = NULL,
            };
            g_array_append_val(out, sp);
            p = q + 1;
          }
        }
      }
    }
    pos = (le < fm_close_line_start) ? (le + 1) : fm_close_line_start;
    line++;
  }
}

static gboolean try_parse_wikilink(const char *s, gsize len, gsize pos, guint line,
                                   BtLinkSpan *out_sp, gsize *out_next)
{
  if (pos + 4 > len || s[pos] != '[' || s[pos + 1] != '[')
    return FALSE;
  if (pos > 0 && (s[pos - 1] == '!' || is_escaped(s, pos)))
    return FALSE;

  gsize t0 = pos + 2;
  gsize q = t0;
  gsize t_end = 0;
  gboolean closed = FALSE;

  while (q + 1 < len && s[q] != '\n' && s[q] != '\r') {
    if (s[q] == '[')
      return FALSE;
    if (s[q] == ']') {
      if (s[q + 1] == ']') {
        t_end = q;
        closed = TRUE;
        break;
      }
      return FALSE;
    }
    if (q - t0 > BT_LINK_MAX_TARGET_BYTES)
      return FALSE;
    q++;
  }
  if (!closed || t_end == t0 || (t_end - t0) > BT_LINK_MAX_TARGET_BYTES)
    return FALSE;

  /* Ignore pure whitespace targets ([[]], [[  ]]) except if they carry an anchor. */
  gboolean all_ws = TRUE;
  for (gsize k = t0; k < t_end; k++) {
    if (!is_hspace(s[k])) {
      all_ws = FALSE;
      break;
    }
  }
  if (all_ws)
    return FALSE;

  out_sp->start = pos;
  out_sp->end = t_end + 2;
  out_sp->target_start = t0;
  out_sp->target_end = t_end;
  out_sp->line_no = line;
  out_sp->syntax = BT_LINK_SYNTAX_WIKILINK;
  out_sp->raw_target = g_strndup(s + t0, t_end - t0);
  out_sp->md_label = NULL;
  *out_next = t_end + 2;
  return TRUE;
}

static gboolean try_parse_md_link(const char *s, gsize len, gsize pos, guint line,
                                  BtLinkSpan *out_sp, gsize *out_next)
{
  if (pos + 4 > len || s[pos] != '[')
    return FALSE;
  if (pos + 1 < len && s[pos + 1] == '[')
    return FALSE;
  if (pos > 0 && (s[pos - 1] == '!' || is_escaped(s, pos)))
    return FALSE;

  gsize l0 = pos + 1;
  gsize q = l0;
  int ldepth = 1;
  while (q < len && s[q] != '\n' && s[q] != '\r') {
    if (s[q] == '\\' && q + 1 < len) {
      q += 2;
      continue;
    }
    if (s[q] == '[')
      ldepth++;
    else if (s[q] == ']') {
      ldepth--;
      if (ldepth == 0)
        break;
    }
    q++;
  }
  if (ldepth != 0 || q + 1 >= len || s[q + 1] != '(')
    return FALSE;

  gsize l_end = q;
  gsize p = q + 2;
  while (p < len && is_hspace(s[p]))
    p++;
  if (p >= len || s[p] == '\n' || s[p] == '\r')
    return FALSE;

  gsize d0 = 0, d_end = 0, close_paren = 0;
  if (s[p] == '<') {
    d0 = p + 1;
    p = d0;
    while (p < len && s[p] != '>' && s[p] != '\n' && s[p] != '\r') {
      if (s[p] == '\\' && p + 1 < len) {
        p += 2;
        continue;
      }
      p++;
    }
    if (p >= len || s[p] != '>')
      return FALSE;
    d_end = p;
    p++;
    while (p < len && is_hspace(s[p]))
      p++;
    if (p < len && (s[p] == '"' || s[p] == '\'')) {
      char qch = s[p++];
      while (p < len && s[p] != qch && s[p] != '\n' && s[p] != '\r') {
        if (s[p] == '\\' && p + 1 < len)
          p += 2;
        else
          p++;
      }
      if (p < len && s[p] == qch)
        p++;
      while (p < len && is_hspace(s[p]))
        p++;
    }
    if (p >= len || s[p] != ')')
      return FALSE;
    close_paren = p;
  } else {
    d0 = p;
    int pdepth = 1;
    while (p < len && s[p] != '\n' && s[p] != '\r' && !is_hspace(s[p])) {
      if (s[p] == '\\' && p + 1 < len) {
        p += 2;
        continue;
      }
      if (s[p] == '(')
        pdepth++;
      else if (s[p] == ')') {
        pdepth--;
        if (pdepth == 0) {
          d_end = p;
          close_paren = p;
          break;
        }
      }
      p++;
    }
    if (close_paren == 0) {
      d_end = p;
      while (p < len && is_hspace(s[p]))
        p++;
      if (p < len && (s[p] == '"' || s[p] == '\'')) {
        char qch = s[p++];
        while (p < len && s[p] != qch && s[p] != '\n' && s[p] != '\r') {
          if (s[p] == '\\' && p + 1 < len)
            p += 2;
          else
            p++;
        }
        if (p >= len || s[p] != qch)
          return FALSE;
        p++;
        while (p < len && is_hspace(s[p]))
          p++;
      }
      if (p >= len || s[p] != ')')
        return FALSE;
      close_paren = p;
    }
  }

  if (d_end <= d0 || (d_end - d0) > BT_LINK_MAX_URL_BYTES)
    return FALSE;

  out_sp->start = pos;
  out_sp->end = close_paren + 1;
  out_sp->target_start = d0;
  out_sp->target_end = d_end;
  out_sp->line_no = line;
  out_sp->syntax = BT_LINK_SYNTAX_MARKDOWN;
  out_sp->raw_target = g_strndup(s + d0, d_end - d0);
  out_sp->md_label = g_strndup(s + l0, l_end - l0);
  *out_next = close_paren + 1;
  return TRUE;
}

static gboolean try_parse_bare_url(const char *s, gsize len, gsize pos, guint line,
                                   BtLinkSpan *out_sp, gsize *out_next)
{
  gsize scheme_len = 0;
  if (pos + 8 < len && memcmp(s + pos, "https://", 8) == 0)
    scheme_len = 8;
  else if (pos + 7 < len && memcmp(s + pos, "http://", 7) == 0)
    scheme_len = 7;
  else
    return FALSE;

  if (pos > 0) {
    char prev = s[pos - 1];
    if (!is_eol_or_space(prev) && prev != '(' && prev != '<' && prev != '"' && prev != '\'')
      return FALSE;
  }

  gsize q = pos + scheme_len;
  while (q < len && !is_eol_or_space(s[q]) && s[q] != '<' && s[q] != '>')
    q++;

  /* Trim trailing punctuation; keep balanced ')' inside URL. */
  while (q > pos + scheme_len) {
    char last = s[q - 1];
    if (last == '.' || last == ',' || last == ';' || last == ':' ||
        last == '!' || last == '?' || last == '\'' || last == '"') {
      q--;
    } else if (last == ')') {
      int open_c = 0, close_c = 0;
      for (gsize k = pos; k < q; k++) {
        if (s[k] == '(') open_c++;
        else if (s[k] == ')') close_c++;
      }
      if (close_c > open_c)
        q--;
      else
        break;
    } else {
      break;
    }
  }

  if (q <= pos + scheme_len || (q - pos) > BT_LINK_MAX_URL_BYTES)
    return FALSE;

  out_sp->start = pos;
  out_sp->end = q;
  out_sp->target_start = pos;
  out_sp->target_end = q;
  out_sp->line_no = line;
  out_sp->syntax = BT_LINK_SYNTAX_BARE_URL;
  out_sp->raw_target = g_strndup(s + pos, q - pos);
  out_sp->md_label = NULL;
  *out_next = q;
  return TRUE;
}

static void scan_links_internal(const char *s, gsize len, gsize limit_end,
                                gsize start_pos, guint start_line,
                                const GArray *mspans, GArray *out)
{
  gsize pos = start_pos;
  guint line = start_line;
  guint math_idx = 0;
  while (mspans != NULL && math_idx < mspans->len &&
         g_array_index(mspans, BtMathSpan, math_idx).end <= start_pos)
    math_idx++;

  gboolean at_line_start = TRUE;
  gboolean prev_line_blank = TRUE;
  gboolean in_fence = FALSE;
  char fence_ch = 0;
  guint fence_len = 0;

  while (pos < limit_end) {
    /* Advance math cursor past any span ending <= pos. */
    while (mspans != NULL && math_idx < mspans->len &&
           g_array_index(mspans, BtMathSpan, math_idx).end <= pos)
      math_idx++;

    /* Never recognize links inside math spans (§A.2, §B.2.4). */
    if (mspans != NULL && math_idx < mspans->len) {
      const BtMathSpan *ms = &g_array_index(mspans, BtMathSpan, math_idx);
      if (pos >= ms->start && pos < ms->end) {
        for (gsize k = pos; k < ms->end && k < limit_end; k++) {
          if (s[k] == '\n')
            line++;
        }
        pos = MIN(ms->end, limit_end);
        at_line_start = (pos > 0 && s[pos - 1] == '\n');
        math_idx++;
        continue;
      }
    }

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

      if (prev_line_blank && bq == 0 && spaces >= 4 && !starts_list_marker(s, cs, le)) {
        pos = (le < limit_end) ? (le + 1) : limit_end;
        line++;
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

    /* Inline code span: exact N backtick match within same paragraph. */
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

    /* HTML comments <!-- ... --> */
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

    /* Wikilink [[...]] */
    if (c == '[' && pos + 1 < len && s[pos + 1] == '[') {
      BtLinkSpan sp;
      gsize next_pos = pos + 2;
      if (try_parse_wikilink(s, len, pos, line, &sp, &next_pos)) {
        g_array_append_val(out, sp);
        pos = next_pos;
        continue;
      }
      /* Advance past any [[ so inner '[' doesn't pretend to be [text](url). */
      pos += 2;
      continue;
    }

    /* Markdown link [text](dest) */
    if (c == '[') {
      BtLinkSpan sp;
      gsize next_pos = pos + 1;
      if (try_parse_md_link(s, len, pos, line, &sp, &next_pos)) {
        g_array_append_val(out, sp);
        pos = next_pos;
        continue;
      }
    }

    /* Bare URL http:// or https:// */
    if (c == 'h') {
      BtLinkSpan sp;
      gsize next_pos = pos + 1;
      if (try_parse_bare_url(s, len, pos, line, &sp, &next_pos)) {
        g_array_append_val(out, sp);
        pos = next_pos;
        continue;
      }
    }

    pos++;
  }
}

GArray *bt_link_scan(const char *text, gsize len)
{
  GArray *out = new_span_array();
  if (text == NULL || len == 0)
    return out;

  gsize start_pos = 0;
  if (len >= 3 && (guchar)text[0] == 0xEFu && (guchar)text[1] == 0xBBu && (guchar)text[2] == 0xBFu)
    start_pos = 3;

  gsize body_pos = start_pos;
  gsize fm_close_line = 0;
  guint line = 1;
  if (match_frontmatter_bounds(text, len, start_pos, &body_pos, &fm_close_line, &line)) {
    scan_frontmatter_links(text, start_pos, fm_close_line, out);
    line += 1;
  } else {
    body_pos = start_pos;
    line = 1;
  }

  GArray *mspans = bt_math_scan(text, len, NULL);
  scan_links_internal(text, len, len, body_pos, line, mspans, out);
  g_array_unref(mspans);
  return out;
}

GArray *bt_link_scan_range(const char *text, gsize len, gsize range_start, gsize range_end,
                           gsize *out_block_start, gsize *out_block_end)
{
  GArray *out = new_span_array();
  if (text == NULL || len == 0) {
    if (out_block_start) *out_block_start = 0;
    if (out_block_end) *out_block_end = 0;
    return out;
  }
  if (range_start > len) range_start = len;
  if (range_end > len) range_end = len;
  if (range_end < range_start) range_end = range_start;

  gsize bom = (len >= 3 && (guchar)text[0] == 0xEFu && (guchar)text[1] == 0xBBu &&
               (guchar)text[2] == 0xBFu)
                  ? 3
                  : 0;
  gsize body_pos = bom;
  gsize fm_close_line = 0;
  guint fm_lines = 0;
  gboolean has_fm = match_frontmatter_bounds(text, len, bom, &body_pos, &fm_close_line, &fm_lines);
  guint body_line = has_fm ? (fm_lines + 1u) : 1u;

  if (has_fm && range_start < body_pos) {
    scan_frontmatter_links(text, bom, fm_close_line, out);
    if (range_end <= body_pos) {
      if (out_block_start) *out_block_start = 0;
      if (out_block_end) *out_block_end = body_pos;
      return out;
    }
  }

  gsize win_start = len;
  gsize win_end = len;
  guint win_line = body_line;
  gsize pos = body_pos;
  guint line = body_line;
  gboolean in_fence = FALSE;
  char fence_ch = 0;
  guint fence_len = 0;
  gboolean in_display = FALSE;
  gboolean in_comment = FALSE;
  gsize chunk_start = body_pos;
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
      if (!in_display && !in_comment && spaces <= 3 && parse_fence_open(text, cs, le, &fch, &flen)) {
        in_fence = TRUE;
        fence_ch = fch;
        fence_len = flen;
      } else {
        for (gsize i = pos; i < le; i++) {
          if (in_comment) {
            if (i + 3 <= le && memcmp(text + i, "-->", 3) == 0) {
              in_comment = FALSE;
              i += 2;
            }
          } else if (!in_display && i + 4 <= le && memcmp(text + i, "<!--", 4) == 0) {
            in_comment = TRUE;
            i += 3;
          } else if (i + 1 < le && text[i] == '$' && text[i + 1] == '$' && !is_escaped(text, i)) {
            in_display = !in_display;
            i++;
          }
        }
      }
    } else if (!in_display && !in_comment) {
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
  if (has_fm && range_start < body_pos)
    win_start = body_pos;

  GArray *mspans = bt_math_scan(text, win_end, NULL);
  scan_links_internal(text, win_end, win_end, win_start, win_line, mspans, out);
  g_array_unref(mspans);

  if (out_block_start) *out_block_start = has_fm && range_start < body_pos ? 0 : win_start;
  if (out_block_end) *out_block_end = win_end;
  return out;
}

gboolean bt_link_span_at(const GArray *spans, gsize byte_offset, BtLinkSpan *out_span)
{
  if (spans == NULL || spans->len == 0)
    return FALSE;
  guint lo = 0, hi = spans->len;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    const BtLinkSpan *sp = &g_array_index(spans, BtLinkSpan, mid);
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

/* ----------------------------------------------------- BtIndexSnapshot ---- */

typedef struct {
  gchar *relpath;
  gboolean is_dir;
  gboolean is_note;
  gchar *title;
  GPtrArray *aliases;
} SnapEntry;

struct BtIndexSnapshot {
  GPtrArray *entries; /* SnapEntry* */
  GHashTable *by_relpath;
  GHashTable *by_base;
  GHashTable *by_stem;
  GHashTable *by_title;
  GHashTable *by_alias;
  gboolean case_sensitive;
  gboolean finalized;
  const BtVault *vault;
};

static void snap_entry_free(gpointer p)
{
  SnapEntry *e = p;
  if (!e) return;
  g_free(e->relpath);
  g_free(e->title);
  if (e->aliases)
    g_ptr_array_unref(e->aliases);
  g_free(e);
}

static GHashTable *make_idx_map(void)
{
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
}

static void add_idx_map(GHashTable *map, gchar *key_owned, guint entry_idx)
{
  GArray *arr = g_hash_table_lookup(map, key_owned);
  if (arr == NULL) {
    arr = g_array_new(FALSE, FALSE, sizeof(guint));
    g_hash_table_insert(map, key_owned, arr);
  } else {
    g_free(key_owned);
  }
  for (guint i = 0; i < arr->len; i++) {
    if (g_array_index(arr, guint, i) == entry_idx)
      return;
  }
  g_array_append_val(arr, entry_idx);
}

BtIndexSnapshot *bt_index_snapshot_new(void)
{
  BtIndexSnapshot *s = g_new0(BtIndexSnapshot, 1);
  s->entries = g_ptr_array_new_with_free_func(snap_entry_free);
  s->by_relpath = make_idx_map();
  s->by_base = make_idx_map();
  s->by_stem = make_idx_map();
  s->by_title = make_idx_map();
  s->by_alias = make_idx_map();
#ifdef G_OS_WIN32
  s->case_sensitive = FALSE;
#else
  s->case_sensitive = TRUE;
#endif
  return s;
}

void bt_index_snapshot_free(BtIndexSnapshot *snap)
{
  if (snap == NULL)
    return;
  g_ptr_array_unref(snap->entries);
  g_hash_table_destroy(snap->by_relpath);
  g_hash_table_destroy(snap->by_base);
  g_hash_table_destroy(snap->by_stem);
  g_hash_table_destroy(snap->by_title);
  g_hash_table_destroy(snap->by_alias);
  g_free(snap);
}

void bt_index_snapshot_set_case_sensitive(BtIndexSnapshot *snap, gboolean case_sensitive)
{
  g_return_if_fail(snap != NULL);
  snap->case_sensitive = case_sensitive;
  snap->finalized = FALSE;
}

void bt_index_snapshot_set_vault(BtIndexSnapshot *snap, const BtVault *vault)
{
  g_return_if_fail(snap != NULL);
  snap->vault = vault;
}

void bt_index_snapshot_add_entry(BtIndexSnapshot *snap, const char *relpath, gboolean is_dir,
                                 const char *title, const char *const *aliases)
{
  g_return_if_fail(snap != NULL && relpath != NULL && relpath[0] != '\0');
  SnapEntry *e = g_new0(SnapEntry, 1);
  e->relpath = g_strdup(relpath);
  for (gchar *p = e->relpath; *p != '\0'; p++)
    if (*p == '\\') *p = '/';
  gsize n = strlen(e->relpath);
  while (n > 1 && e->relpath[n - 1] == '/') {
    e->relpath[n - 1] = '\0';
    n--;
  }
  e->is_dir = is_dir;
  e->is_note = !is_dir && is_note_ext(e->relpath);
  e->title = (title != NULL && title[0] != '\0') ? g_strdup(title) : NULL;
  if (aliases != NULL) {
    e->aliases = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; aliases[i] != NULL; i++) {
      if (aliases[i][0] != '\0')
        g_ptr_array_add(e->aliases, g_strdup(aliases[i]));
    }
  }
  g_ptr_array_add(snap->entries, e);
  snap->finalized = FALSE;
}

void bt_index_snapshot_finalize(BtIndexSnapshot *snap)
{
  g_return_if_fail(snap != NULL);
  g_hash_table_remove_all(snap->by_relpath);
  g_hash_table_remove_all(snap->by_base);
  g_hash_table_remove_all(snap->by_stem);
  g_hash_table_remove_all(snap->by_title);
  g_hash_table_remove_all(snap->by_alias);

  for (guint i = 0; i < snap->entries->len; i++) {
    const SnapEntry *e = g_ptr_array_index(snap->entries, i);
    add_idx_map(snap->by_relpath, normalize_key(e->relpath, snap->case_sensitive), i);
    if (!e->is_dir) {
      const char *base = path_basename_ptr(e->relpath);
      add_idx_map(snap->by_base, normalize_key(base, snap->case_sensitive), i);
      if (e->is_note) {
        g_autofree gchar *stem = strip_note_ext(base);
        add_idx_map(snap->by_stem, normalize_key(stem, snap->case_sensitive), i);
      }
      if (e->title != NULL)
        add_idx_map(snap->by_title, normalize_key(e->title, FALSE), i);
      if (e->aliases != NULL) {
        for (guint a = 0; a < e->aliases->len; a++) {
          const char *al = g_ptr_array_index(e->aliases, a);
          add_idx_map(snap->by_alias, normalize_key(al, FALSE), i);
        }
      }
    }
  }
  snap->finalized = TRUE;
}

static void walk_vault_for_snapshot(const BtVault *vault, const char *abs_dir,
                                    const char *rel_dir, guint depth, BtIndexSnapshot *snap)
{
  if (depth > 64)
    return;
  GPtrArray *entries = bt_platform_list_dir(abs_dir, NULL);
  if (entries == NULL)
    return;

  for (guint i = 0; i < entries->len; i++) {
    const BtDirent *d = g_ptr_array_index(entries, i);
    g_autofree gchar *child_rel =
        (rel_dir[0] != '\0') ? g_strconcat(rel_dir, "/", d->name, NULL) : g_strdup(d->name);
    if (vault_is_ignored_path(vault, child_rel))
      continue;
    g_autofree gchar *full = g_build_filename(abs_dir, d->name, NULL);

    if (d->kind == BT_DIRENT_DIR) {
      bt_index_snapshot_add_entry(snap, child_rel, TRUE, NULL, NULL);
      walk_vault_for_snapshot(vault, full, child_rel, depth + 1, snap);
    } else if (d->kind == BT_DIRENT_FILE) {
      if (is_note_ext(child_rel)) {
        g_autofree gchar *raw = NULL;
        gsize len = 0;
        if (vault_read(vault, child_rel, &raw, &len, NULL)) {
          BtFrontmatter *fm = bt_frontmatter_parse(raw, len);
          g_autoptr(GPtrArray) alias_ptrs = g_ptr_array_new();
          if (fm->aliases != NULL) {
            for (guint a = 0; a < fm->aliases->len; a++)
              g_ptr_array_add(alias_ptrs, g_ptr_array_index(fm->aliases, a));
          }
          g_ptr_array_add(alias_ptrs, NULL);
          const char *title = fm->title;
          bt_index_snapshot_add_entry(snap, child_rel, FALSE, title,
                                      (const char *const *)alias_ptrs->pdata);
          bt_frontmatter_free(fm);
        } else {
          bt_index_snapshot_add_entry(snap, child_rel, FALSE, NULL, NULL);
        }
      } else {
        bt_index_snapshot_add_entry(snap, child_rel, FALSE, NULL, NULL);
      }
    }
  }
  g_ptr_array_unref(entries);
}

BtIndexSnapshot *bt_index_snapshot_from_vault(const BtVault *vault)
{
  g_return_val_if_fail(vault != NULL, NULL);
  BtIndexSnapshot *snap = bt_index_snapshot_new();
  bt_index_snapshot_set_vault(snap, vault);
  walk_vault_for_snapshot(vault, vault_root(vault), "", 0, snap);
  bt_index_snapshot_finalize(snap);
  return snap;
}

/* ----------------------------------------------------- resolver §A.3 ----- */

static BtLinkResolution *make_blocked(BtLinkBlockReason reason, BtLinkKind kind,
                                      const char *display, const char *anchor,
                                      const char *block_id)
{
  BtLinkResolution *r = g_new0(BtLinkResolution, 1);
  r->status = BT_LINK_BLOCKED;
  r->kind = kind;
  r->block_reason = reason;
  r->display = display ? g_strdup(display) : NULL;
  r->anchor = anchor ? g_strdup(anchor) : NULL;
  r->block_id = block_id ? g_strdup(block_id) : NULL;
  return r;
}

static gboolean has_ctrl_chars(const char *s, gsize len)
{
  for (gsize i = 0; i < len; i++) {
    guchar u = (guchar)s[i];
    if (u < 0x20u || u == 0x7Fu)
      return TRUE;
  }
  return FALSE;
}

static gboolean is_reserved_device_name(const char *seg)
{
  static const char *const names[] = {
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
    "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL
  };
  gsize stem = strcspn(seg, ".");
  gsize end = stem;
  while (end > 0 && seg[end - 1] == ' ')
    end--;
  for (guint i = 0; names[i] != NULL; i++) {
    if (strlen(names[i]) == end && g_ascii_strncasecmp(seg, names[i], end) == 0)
      return TRUE;
  }
  return FALSE;
}

/* Checks URI scheme ^[A-Za-z][A-Za-z0-9+.-]*: (at least 2 chars so C: isn't matched).
 * Rejects <filename>.<ext>:<stream> (Windows ADS like a.md:stream) when there is a '.'
 * before ':' and no "://" authority marker. */
static gboolean detect_scheme(const char *s, gchar **out_scheme_lower)
{
  if (!g_ascii_isalpha(s[0]))
    return FALSE;
  gsize i = 1;
  gboolean saw_dot = FALSE;
  while (s[i] != '\0' && (g_ascii_isalnum(s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) {
    if (s[i] == '.')
      saw_dot = TRUE;
    i++;
  }
  if (i >= 2 && s[i] == ':') {
    if (saw_dot && !(s[i + 1] == '/' && s[i + 2] == '/'))
      return FALSE;
    *out_scheme_lower = g_ascii_strdown(s, (gssize)i);
    return TRUE;
  }
  return FALSE;
}

static int hex_val(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static gboolean percent_decode_path(const char *in, gchar **out, gboolean *out_ctrl)
{
  GString *buf = g_string_sized_new(strlen(in));
  *out_ctrl = FALSE;
  for (const char *p = in; *p != '\0'; p++) {
    if (p[0] == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
      guchar b = (guchar)((hex_val(p[1]) << 4) | hex_val(p[2]));
      if (b < 0x20u || b == 0x7Fu) {
        *out_ctrl = TRUE;
        g_string_free(buf, TRUE);
        *out = NULL;
        return FALSE;
      }
      g_string_append_c(buf, (char)b);
      p += 2;
    } else {
      g_string_append_c(buf, *p);
    }
  }
  if (!g_utf8_validate(buf->str, (gssize)buf->len, NULL)) {
    g_string_free(buf, TRUE);
    *out = NULL;
    return FALSE;
  }
  *out = g_string_free(buf, FALSE);
  return TRUE;
}

static guint count_slashes(const char *p)
{
  guint c = 0;
  for (const char *q = p; *q != '\0'; q++)
    if (*q == '/') c++;
  return c;
}

typedef struct {
  const SnapEntry *entry;
  gboolean same_dir;
  gboolean in_source_subtree;
  gboolean has_dot_segment;
  guint depth;
} RankedCand;

static gboolean relpath_has_dot_segment(const char *relpath)
{
  if (relpath == NULL)
    return FALSE;
  const char *p = relpath;
  while (*p != '\0') {
    if (p[0] == '.' && p[1] != '/' && p[1] != '\0')
      return TRUE;
    const char *sl = strchr(p, '/');
    if (sl == NULL)
      break;
    p = sl + 1;
  }
  return FALSE;
}

static gboolean dir_is_in_subtree(const char *cand_dir, const char *src_dir, gboolean case_sensitive)
{
  if (src_dir == NULL || src_dir[0] == '\0' || cand_dir == NULL || cand_dir[0] == '\0')
    return FALSE;
  gsize sl = strlen(src_dir);
  gsize cl = strlen(cand_dir);
  if (cl <= sl || cand_dir[sl] != '/')
    return FALSE;
  return case_sensitive ? (strncmp(cand_dir, src_dir, sl) == 0)
                        : (g_ascii_strncasecmp(cand_dir, src_dir, sl) == 0);
}

static gint cmp_ranked_cand(gconstpointer a, gconstpointer b)
{
  const RankedCand *ra = a;
  const RankedCand *rb = b;
  if (ra->same_dir != rb->same_dir)
    return ra->same_dir ? -1 : 1;
  if (ra->in_source_subtree != rb->in_source_subtree)
    return ra->in_source_subtree ? -1 : 1;
  if (ra->has_dot_segment != rb->has_dot_segment)
    return ra->has_dot_segment ? 1 : -1;
  if (ra->depth != rb->depth)
    return (ra->depth < rb->depth) ? -1 : 1;
  return strcmp(ra->entry->relpath, rb->entry->relpath);
}

static void collect_matches(const BtIndexSnapshot *snap, GHashTable *map, const char *key_raw,
                            gboolean ci_key, GArray *matched_indices)
{
  if (key_raw == NULL || key_raw[0] == '\0')
    return;
  g_autofree gchar *k = normalize_key(key_raw, ci_key ? FALSE : snap->case_sensitive);
  GArray *arr = g_hash_table_lookup(map, k);
  if (arr == NULL)
    return;
  for (guint i = 0; i < arr->len; i++) {
    guint idx = g_array_index(arr, guint, i);
    gboolean dup = FALSE;
    for (guint j = 0; j < matched_indices->len; j++) {
      if (g_array_index(matched_indices, guint, j) == idx) {
        dup = TRUE;
        break;
      }
    }
    if (!dup)
      g_array_append_val(matched_indices, idx);
  }
}

static void try_stage_exact_and_md(const BtIndexSnapshot *snap, const char *rel_candidate,
                                   gboolean add_md, GArray *matched_indices)
{
  collect_matches(snap, snap->by_relpath, rel_candidate, FALSE, matched_indices);
  if (add_md && !is_note_ext(rel_candidate)) {
    g_autofree gchar *with_md = g_strconcat(rel_candidate, ".md", NULL);
    collect_matches(snap, snap->by_relpath, with_md, FALSE, matched_indices);
    g_autofree gchar *with_markdown = g_strconcat(rel_candidate, ".markdown", NULL);
    collect_matches(snap, snap->by_relpath, with_markdown, FALSE, matched_indices);
  }
}

BtLinkResolution *bt_link_resolve(const BtIndexSnapshot *idx, const char *source_note,
                                  const char *raw_target)
{
  g_return_val_if_fail(idx != NULL, NULL);
  if (raw_target == NULL)
    return NULL;

  /* Strip optional surrounding [[ ]] if caller passed raw wikilink token. */
  g_autofree gchar *raw = NULL;
  gsize raw_len = strlen(raw_target);
  if (raw_len >= 4 && raw_target[0] == '[' && raw_target[1] == '[' &&
      raw_target[raw_len - 2] == ']' && raw_target[raw_len - 1] == ']') {
    raw = g_strndup(raw_target + 2, raw_len - 4);
  } else {
    raw = g_strdup(raw_target);
  }

  /* Length check before parsing (allow URLs up to 2048 bytes, file/note targets 1024). */
  gboolean looks_like_url = (g_ascii_strncasecmp(raw, "http://", 7) == 0 ||
                             g_ascii_strncasecmp(raw, "https://", 8) == 0 ||
                             g_ascii_strncasecmp(raw, "mailto:", 7) == 0);
  gsize rlen = strlen(raw);
  if (has_ctrl_chars(raw, rlen))
    return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_NOTE, raw, NULL, NULL);

  /* Step 1: Split alias at first | (or \|). */
  g_autofree gchar *target_part = NULL;
  g_autofree gchar *alias_part = NULL;
  const char *pipe = strchr(raw, '|');
  if (pipe != NULL) {
    gsize tlen = (gsize)(pipe - raw);
    if (tlen > 0 && raw[tlen - 1] == '\\')
      tlen--;
    target_part = g_strndup(raw, tlen);
    const char *ap = pipe + 1;
    while (is_hspace(*ap)) ap++;
    if (*ap != '\0')
      alias_part = g_strdup(ap);
  } else {
    target_part = g_strdup(raw);
  }

  if (strlen(target_part) > (looks_like_url ? BT_LINK_MAX_URL_BYTES : BT_LINK_MAX_TARGET_BYTES))
    return make_blocked(BT_LINK_BLOCK_TOO_LONG,
                        looks_like_url ? BT_LINK_KIND_URL : BT_LINK_KIND_NOTE,
                        alias_part ? alias_part : target_part, NULL, NULL);

  /* Trim leading spaces ONLY first to detect empty link vs unsafe trailing space/dot. */
  const char *lead_trim = target_part;
  while (is_hspace(*lead_trim))
    lead_trim++;
  if (*lead_trim == '\0')
    return NULL; /* Empty non-link (R31) */

  /* P-3: Template placeholder matching \{\{[^}]*\}\} is PLACEHOLDER, not UNRESOLVED. */
  if (!looks_like_url) {
    const char *open = strstr(lead_trim, "{{");
    if (open != NULL) {
      const char *p = open + 2;
      while (*p != '\0' && *p != '}' && *p != '\r' && *p != '\n')
        p++;
      if (p[0] == '}' && p[1] == '}') {
        BtLinkResolution *r = g_new0(BtLinkResolution, 1);
        r->status = BT_LINK_PLACEHOLDER;
        r->kind = BT_LINK_KIND_NOTE;
        r->display = g_strdup(alias_part ? alias_part : lead_trim);
        return r;
      }
    }
  }

  /* Windows UNC / device prefixes before scheme check. */
  if (g_str_has_prefix(lead_trim, "\\\\") || g_str_has_prefix(lead_trim, "//"))
    return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_NOTE,
                        alias_part ? alias_part : lead_trim, NULL, NULL);

  /* Windows drive-letter prefix (C:\... or C:/...). */
  if (g_ascii_isalpha(lead_trim[0]) && lead_trim[1] == ':' &&
      (lead_trim[2] == '\\' || lead_trim[2] == '/' || lead_trim[2] == '\0'))
    return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_FILE,
                        alias_part ? alias_part : lead_trim, NULL, NULL);

  /* Step 10: Check URI scheme (http, https, mailto -> EXTERNAL; others -> BLOCKED). */
  g_autofree gchar *scheme = NULL;
  if (detect_scheme(lead_trim, &scheme)) {
    g_autofree gchar *trimmed_url = g_strchomp(g_strdup(lead_trim));
    if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0 || strcmp(scheme, "mailto") == 0) {
      if (strlen(trimmed_url) > BT_LINK_MAX_URL_BYTES)
        return make_blocked(BT_LINK_BLOCK_TOO_LONG, BT_LINK_KIND_URL, trimmed_url, NULL, NULL);
      BtLinkResolution *r = g_new0(BtLinkResolution, 1);
      r->status = BT_LINK_EXTERNAL;
      r->kind = BT_LINK_KIND_URL;
      r->url = g_strdup(trimmed_url);
      r->display = g_strdup(alias_part ? alias_part : trimmed_url);
      return r;
    }
    return make_blocked(BT_LINK_BLOCK_SCHEME_NOT_ALLOWED, BT_LINK_KIND_URL,
                        alias_part ? alias_part : trimmed_url, NULL, NULL);
  }

  /* Step 2: Split anchor (#) and block id (^). */
  g_autofree gchar *path_raw = g_strdup(lead_trim);
  g_autofree gchar *anchor = NULL;
  g_autofree gchar *block_id = NULL;
  char *hash = strchr(path_raw, '#');
  char *caret = strchr(path_raw, '^');
  if (caret != NULL) {
    *caret = '\0';
    if (*(caret + 1) != '\0')
      block_id = g_strchomp(g_strdup(caret + 1));
  }
  if (hash != NULL) {
    *hash = '\0';
    if (*(hash + 1) != '\0')
      anchor = g_strchomp(g_strdup(hash + 1));
  }

  /* Alternate Data Stream colon (e.g. a.md:stream). */
  if (strchr(path_raw, ':') != NULL)
    return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_FILE,
                        alias_part ? alias_part : raw, anchor, block_id);

  /* Percent-decode Markdown file paths (e.g. my%20note.md). */
  g_autofree gchar *decoded = NULL;
  gboolean had_ctrl = FALSE;
  if (!percent_decode_path(path_raw, &decoded, &had_ctrl)) {
    return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_FILE,
                        alias_part ? alias_part : raw, anchor, block_id);
  }

  /* Check SAME_NOTE case ([[#Heading]] or [[^block]]). */
  g_autofree gchar *decoded_trim = g_strstrip(g_strdup(decoded));
  if (decoded_trim[0] == '\0') {
    if (anchor != NULL || block_id != NULL) {
      BtLinkResolution *r = g_new0(BtLinkResolution, 1);
      r->status = BT_LINK_RESOLVED;
      r->kind = BT_LINK_KIND_SAME_NOTE;
      r->path = source_note ? g_strdup(source_note) : NULL;
      r->anchor = g_strdup(anchor);
      r->block_id = g_strdup(block_id);
      r->display = g_strdup(alias_part ? alias_part : (anchor ? anchor : block_id));
      return r;
    }
    return NULL;
  }

  /* Normalize '\' to '/' and collapse '//'. */
  GString *norm = g_string_sized_new(strlen(decoded));
  for (const char *p = decoded; *p != '\0'; p++) {
    char c = (*p == '\\') ? '/' : *p;
    if (c == '/' && norm->len > 0 && norm->str[norm->len - 1] == '/')
      continue;
    g_string_append_c(norm, c);
  }

  /* Leading slash escapes/is absolute outside relative vault contract. */
  if (norm->len > 0 && norm->str[0] == '/') {
    g_string_free(norm, TRUE);
    return make_blocked(BT_LINK_BLOCK_ESCAPES_VAULT, BT_LINK_KIND_NOTE,
                        alias_part ? alias_part : lead_trim, anchor, block_id);
  }

  /* Validate each raw segment for trailing '.' / ' ' (R26) and reserved device names (R25). */
  gchar **raw_segs = g_strsplit(norm->str, "/", -1);
  for (guint i = 0; raw_segs[i] != NULL; i++) {
    const char *seg = raw_segs[i];
    if (seg[0] == '\0')
      continue;
    gsize slen = strlen(seg);
    if (strcmp(seg, ".") != 0 && strcmp(seg, "..") != 0) {
      if (seg[slen - 1] == ' ' || seg[slen - 1] == '\t' || seg[slen - 1] == '.') {
        g_strfreev(raw_segs);
        g_string_free(norm, TRUE);
        return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_FILE,
                            alias_part ? alias_part : lead_trim, anchor, block_id);
      }
      if (is_reserved_device_name(seg)) {
        g_strfreev(raw_segs);
        g_string_free(norm, TRUE);
        return make_blocked(BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_KIND_FILE,
                            alias_part ? alias_part : lead_trim, anchor, block_id);
      }
    }
  }

  /* Lexical '..' normalization: if source_note has a directory and target has '.' or '..',
   * resolve from source_dir; otherwise resolve from vault root so '../outside' escapes. */
  gboolean has_dotdot = FALSE;
  gboolean has_explicit_rel = (strcmp(raw_segs[0], ".") == 0 || strcmp(raw_segs[0], "..") == 0);
  for (guint i = 0; raw_segs[i] != NULL; i++) {
    if (strcmp(raw_segs[i], "..") == 0) {
      has_dotdot = TRUE;
      break;
    }
  }
  g_autofree gchar *source_dir = path_dirname_dup(source_note);
  g_autoptr(GPtrArray) seg_stack = g_ptr_array_new_with_free_func(g_free);
  if (has_dotdot && source_dir[0] != '\0') {
    gchar **sd_segs = g_strsplit(source_dir, "/", -1);
    for (guint i = 0; sd_segs[i] != NULL; i++) {
      if (sd_segs[i][0] != '\0')
        g_ptr_array_add(seg_stack, g_strdup(sd_segs[i]));
    }
    g_strfreev(sd_segs);
  }
  for (guint i = 0; raw_segs[i] != NULL; i++) {
    const char *seg = raw_segs[i];
    if (seg[0] == '\0' || strcmp(seg, ".") == 0)
      continue;
    if (strcmp(seg, "..") == 0) {
      if (seg_stack->len == 0) {
        g_strfreev(raw_segs);
        g_string_free(norm, TRUE);
        return make_blocked(BT_LINK_BLOCK_ESCAPES_VAULT, BT_LINK_KIND_NOTE,
                            alias_part ? alias_part : lead_trim, anchor, block_id);
      }
      g_ptr_array_set_size(seg_stack, seg_stack->len - 1);
    } else {
      g_ptr_array_add(seg_stack, g_strdup(seg));
    }
  }
  g_strfreev(raw_segs);
  g_string_free(norm, TRUE);

  GString *resolved_rel_buf = g_string_new(NULL);
  for (guint i = 0; i < seg_stack->len; i++) {
    if (i > 0) g_string_append_c(resolved_rel_buf, '/');
    g_string_append(resolved_rel_buf, (const char *)g_ptr_array_index(seg_stack, i));
  }
  g_autofree gchar *clean_target = g_string_free(resolved_rel_buf, FALSE);
  if (clean_target[0] == '\0')
    return NULL;

  /* Run Stages 1..5 on snap (finalize lazily if caller forgot). */
  BtIndexSnapshot *mut_snap = (BtIndexSnapshot *)idx;
  if (!mut_snap->finalized)
    bt_index_snapshot_finalize(mut_snap);

  g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(guint));

  /* Stage 1: exact vault-relative path */
  try_stage_exact_and_md(idx, clean_target, FALSE, matches);

  /* Stage 2: same path with .md appended */
  if (matches->len == 0)
    try_stage_exact_and_md(idx, clean_target, TRUE, matches);

  /* Stage 3: path relative to source_note directory for explicit/folder-qualified paths */
  if (matches->len == 0 && source_dir[0] != '\0' && !has_dotdot &&
      (has_explicit_rel || strchr(clean_target, '/') != NULL)) {
    g_autofree gchar *rel_to_src = g_strconcat(source_dir, "/", clean_target, NULL);
    try_stage_exact_and_md(idx, rel_to_src, TRUE, matches);
  }

  /* Stage 4: basename / stem match anywhere in vault */
  if (matches->len == 0 && strchr(clean_target, '/') == NULL) {
    collect_matches(idx, idx->by_base, clean_target, FALSE, matches);
    if (matches->len == 0 && !is_note_ext(clean_target))
      collect_matches(idx, idx->by_stem, clean_target, FALSE, matches);
  }

  /* Stage 5: frontmatter title match, then aliases match (case-insensitive NFC) */
  if (matches->len == 0) {
    collect_matches(idx, idx->by_title, clean_target, TRUE, matches);
    if (matches->len == 0)
      collect_matches(idx, idx->by_alias, clean_target, TRUE, matches);
  }

  /* Step 12: Symlink / junction escape check through vault_resolve() if vault attached. */
  if (idx->vault != NULL) {
    if (matches->len > 0) {
      for (guint i = 0; i < matches->len; i++) {
        const SnapEntry *ce = g_ptr_array_index(idx->entries, g_array_index(matches, guint, i));
        g_autofree gchar *real = vault_resolve(idx->vault, ce->relpath, NULL);
        if (real == NULL) {
          return make_blocked(BT_LINK_BLOCK_ESCAPES_VAULT, BT_LINK_KIND_FILE,
                              alias_part ? alias_part : clean_target, anchor, block_id);
        }
      }
    } else {
      g_autoptr(GError) verr = NULL;
      g_autofree gchar *chk = vault_resolve(idx->vault, clean_target, &verr);
      if (chk == NULL && verr != NULL && verr->message != NULL &&
          strstr(verr->message, "through a link") != NULL) {
        return make_blocked(BT_LINK_BLOCK_ESCAPES_VAULT, BT_LINK_KIND_FILE,
                            alias_part ? alias_part : clean_target, anchor, block_id);
      }
    }
  }

  BtLinkResolution *res = g_new0(BtLinkResolution, 1);
  res->anchor = g_strdup(anchor);
  res->block_id = g_strdup(block_id);
  res->display = g_strdup(alias_part ? alias_part : lead_trim);

  if (matches->len == 0) {
    res->status = BT_LINK_UNRESOLVED;
    res->kind = BT_LINK_KIND_NOTE;
    return res;
  }

  if (matches->len == 1) {
    const SnapEntry *e = g_ptr_array_index(idx->entries, g_array_index(matches, guint, 0));
    res->status = BT_LINK_RESOLVED;
    res->path = g_strdup(e->relpath);
    res->kind = e->is_dir ? BT_LINK_KIND_DIR : (e->is_note ? BT_LINK_KIND_NOTE : BT_LINK_KIND_FILE);
    return res;
  }

  /* Ambiguous: rank by (same dir, in source subtree, non-dot segment, fewer segments, lexical). */
  g_autoptr(GArray) ranked = g_array_new(FALSE, FALSE, sizeof(RankedCand));
  for (guint i = 0; i < matches->len; i++) {
    const SnapEntry *e = g_ptr_array_index(idx->entries, g_array_index(matches, guint, i));
    g_autofree gchar *cdir = path_dirname_dup(e->relpath);
    gboolean sdir = FALSE;
    if (source_dir[0] == '\0' && cdir[0] == '\0')
      sdir = TRUE;
    else if (source_dir[0] != '\0' && cdir[0] != '\0')
      sdir = idx->case_sensitive ? (strcmp(source_dir, cdir) == 0)
                                 : (g_ascii_strcasecmp(source_dir, cdir) == 0);
    RankedCand rc = {
      .entry = e,
      .same_dir = sdir,
      .in_source_subtree = dir_is_in_subtree(cdir, source_dir, idx->case_sensitive),
      .has_dot_segment = relpath_has_dot_segment(e->relpath),
      .depth = count_slashes(e->relpath),
    };
    g_array_append_val(ranked, rc);
  }
  g_array_sort(ranked, cmp_ranked_cand);

  res->status = BT_LINK_AMBIGUOUS;
  res->candidates = g_new0(char *, ranked->len + 1);
  for (guint i = 0; i < ranked->len; i++)
    res->candidates[i] = g_strdup(g_array_index(ranked, RankedCand, i).entry->relpath);
  const SnapEntry *best = g_array_index(ranked, RankedCand, 0).entry;
  res->path = g_strdup(best->relpath);
  res->kind = best->is_dir ? BT_LINK_KIND_DIR
                           : (best->is_note ? BT_LINK_KIND_NOTE : BT_LINK_KIND_FILE);
  return res;
}

void bt_link_resolution_free(BtLinkResolution *r)
{
  if (r == NULL)
    return;
  g_free(r->path);
  g_strfreev(r->candidates);
  g_free(r->anchor);
  g_free(r->block_id);
  g_free(r->display);
  g_free(r->url);
  g_free(r);
}

/* ----------------------------------------------------- A.9 open policy ---- */

static const char *const BLOCKED_EXTS[] = {
  "exe", "com", "bat", "cmd", "ps1", "psm1", "psd1", "vbs", "vbe", "js", "jse",
  "wsf", "wsh", "msi", "msp", "scr", "pif", "lnk", "url", "reg", "hta", "cpl",
  "msc", "jar", "dll", "sys", "inf", "appx", "msix", "ps1xml", "gadget",
  "application", NULL
};

static const char *const ALLOWED_EXTS[] = {
  "pdf", "png", "jpg", "jpeg", "gif", "webp", "svg", "txt", "csv", "tsv",
  "json", "md", "markdown", "mp3", "wav", "ogg", "mp4", "webm", NULL
};

static gboolean ext_in_list(const char *ext, const char *const *list)
{
  if (ext == NULL || ext[0] == '\0')
    return FALSE;
  for (guint i = 0; list[i] != NULL; i++) {
    if (g_ascii_strcasecmp(ext, list[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static gchar *extract_clean_ext(const char *path)
{
  if (path == NULL)
    return NULL;
  const char *base = path_basename_ptr(path);
  g_autofree gchar *copy = g_strdup(base);
  gsize n = strlen(copy);
  while (n > 0 && (copy[n - 1] == '.' || copy[n - 1] == ' ' || copy[n - 1] == '\t'))
    copy[--n] = '\0';
  const char *dot = strrchr(copy, '.');
  if (dot == NULL || dot[1] == '\0')
    return NULL;
  return g_ascii_strdown(dot + 1, -1);
}

static gchar *strip_url_userinfo(const char *url, gboolean *out_had_userinfo)
{
  *out_had_userinfo = FALSE;
  if (url == NULL)
    return NULL;
  const char *scheme_sep = strstr(url, "://");
  if (scheme_sep == NULL)
    return g_strdup(url);
  const char *auth = scheme_sep + 3;
  const char *slash = strpbrk(auth, "/?#");
  const char *at = memchr(auth, '@', slash ? (size_t)(slash - auth) : strlen(auth));
  if (at == NULL)
    return g_strdup(url);
  *out_had_userinfo = TRUE;
  GString *clean = g_string_new_len(url, (gssize)(auth - url));
  g_string_append(clean, at + 1);
  return g_string_free(clean, FALSE);
}

const char *bt_link_block_reason_message(BtLinkBlockReason reason)
{
  switch (reason) {
  case BT_LINK_BLOCK_ESCAPES_VAULT:      return "Blocked: path escapes vault root";
  case BT_LINK_BLOCK_UNSAFE_PATH:        return "Blocked: unsafe path or executable extension";
  case BT_LINK_BLOCK_SCHEME_NOT_ALLOWED: return "Blocked: URL scheme is not allowed";
  case BT_LINK_BLOCK_TOO_LONG:           return "Blocked: link target exceeds length limit";
  case BT_LINK_BLOCK_NONE:
  default:                               return "Allowed";
  }
}

const char *bt_link_status_name(BtLinkStatus status)
{
  switch (status) {
  case BT_LINK_RESOLVED:    return "resolved";
  case BT_LINK_AMBIGUOUS:   return "ambiguous";
  case BT_LINK_UNRESOLVED:  return "unresolved";
  case BT_LINK_EXTERNAL:    return "external";
  case BT_LINK_PLACEHOLDER: return "placeholder";
  case BT_LINK_BLOCKED:
  default:                  return "blocked";
  }
}

const char *bt_link_kind_name(BtLinkKind kind)
{
  switch (kind) {
  case BT_LINK_KIND_NOTE:      return "note";
  case BT_LINK_KIND_FILE:      return "file";
  case BT_LINK_KIND_DIR:       return "dir";
  case BT_LINK_KIND_URL:       return "url";
  case BT_LINK_KIND_SAME_NOTE:
  default:                     return "same_note";
  }
}

void bt_link_check_open_policy(const BtLinkResolution *res, const BtVault *vault,
                               gboolean confirm_external_urls, BtLinkOpenPolicy *out_policy)
{
  g_return_if_fail(out_policy != NULL);
  memset(out_policy, 0, sizeof(*out_policy));

  if (res == NULL) {
    out_policy->action = BT_LINK_OPEN_BLOCKED;
    out_policy->block_reason = BT_LINK_BLOCK_UNSAFE_PATH;
    out_policy->reason_msg = g_strdup("Blocked: empty link");
    return;
  }

  if (res->status == BT_LINK_BLOCKED) {
    out_policy->action = BT_LINK_OPEN_BLOCKED;
    out_policy->block_reason = res->block_reason;
    out_policy->reason_msg = g_strdup(bt_link_block_reason_message(res->block_reason));
    return;
  }

  if (res->status == BT_LINK_EXTERNAL || res->kind == BT_LINK_KIND_URL) {
    if (res->url == NULL || strlen(res->url) > BT_LINK_MAX_URL_BYTES) {
      out_policy->action = BT_LINK_OPEN_BLOCKED;
      out_policy->block_reason = BT_LINK_BLOCK_TOO_LONG;
      out_policy->reason_msg = g_strdup(bt_link_block_reason_message(BT_LINK_BLOCK_TOO_LONG));
      return;
    }
    g_autofree gchar *scheme = NULL;
    if (!detect_scheme(res->url, &scheme) ||
        (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0 && strcmp(scheme, "mailto") != 0)) {
      out_policy->action = BT_LINK_OPEN_BLOCKED;
      out_policy->block_reason = BT_LINK_BLOCK_SCHEME_NOT_ALLOWED;
      out_policy->reason_msg = g_strdup(bt_link_block_reason_message(BT_LINK_BLOCK_SCHEME_NOT_ALLOWED));
      return;
    }
    out_policy->safe_display_url = strip_url_userinfo(res->url, &out_policy->has_userinfo);
    out_policy->action = confirm_external_urls ? BT_LINK_OPEN_CONFIRM_URL : BT_LINK_OPEN_ALLOW;
    if (out_policy->has_userinfo)
      out_policy->reason_msg = g_strdup("Warning: URL contains embedded credentials (stripped)");
    return;
  }

  if (res->kind == BT_LINK_KIND_SAME_NOTE) {
    out_policy->action = BT_LINK_OPEN_ALLOW;
    return;
  }

  if (res->path != NULL && vault != NULL) {
    g_autoptr(GError) verr = NULL;
    g_autofree gchar *real = vault_resolve(vault, res->path, &verr);
    if (real == NULL) {
      out_policy->action = BT_LINK_OPEN_BLOCKED;
      out_policy->block_reason = BT_LINK_BLOCK_ESCAPES_VAULT;
      out_policy->reason_msg = g_strdup(bt_link_block_reason_message(BT_LINK_BLOCK_ESCAPES_VAULT));
      return;
    }
  }

  if (res->kind == BT_LINK_KIND_DIR) {
    out_policy->action = BT_LINK_OPEN_CONFIRM_DIR;
    out_policy->reason_msg = g_strdup("Confirm revealing folder in file manager");
    return;
  }

  if (res->kind == BT_LINK_KIND_NOTE) {
    out_policy->action = BT_LINK_OPEN_ALLOW;
    return;
  }

  /* BT_LINK_KIND_FILE */
  g_autofree gchar *ext = extract_clean_ext(res->path);
  if (ext_in_list(ext, BLOCKED_EXTS)) {
    out_policy->action = BT_LINK_OPEN_BLOCKED;
    out_policy->block_reason = BT_LINK_BLOCK_UNSAFE_PATH;
    out_policy->reason_msg = g_strdup_printf("Blocked executable/script extension: .%s", ext);
    return;
  }

  /* Guard trailing dot/space extension bypasses on files too. */
  if (res->path != NULL) {
    const char *base = path_basename_ptr(res->path);
    gsize bl = strlen(base);
    if (bl > 0 && (base[bl - 1] == '.' || base[bl - 1] == ' ' || base[bl - 1] == '\t')) {
      out_policy->action = BT_LINK_OPEN_BLOCKED;
      out_policy->block_reason = BT_LINK_BLOCK_UNSAFE_PATH;
      out_policy->reason_msg = g_strdup("Blocked: unsafe trailing dot or space in filename");
      return;
    }
  }

  if (ext_in_list(ext, ALLOWED_EXTS)) {
    out_policy->action = BT_LINK_OPEN_ALLOW;
    return;
  }

  out_policy->action = BT_LINK_OPEN_CONFIRM_EXT;
  out_policy->reason_msg = g_strdup_printf("Unrecognized extension '.%s': confirm before opening",
                                           ext ? ext : "(none)");
}

void bt_link_open_policy_clear(BtLinkOpenPolicy *policy)
{
  if (policy == NULL)
    return;
  g_free(policy->safe_display_url);
  g_free(policy->reason_msg);
  memset(policy, 0, sizeof(*policy));
}

gboolean bt_link_dispatch_open(const BtLinkResolution *res, const BtVault *vault,
                               gboolean confirmed_by_user, gboolean confirm_external_urls,
                               BtLinkLauncherFn launcher, void *user_data,
                               BtLinkOpenPolicy *out_policy)
{
  BtLinkOpenPolicy local;
  bt_link_check_open_policy(res, vault, confirm_external_urls, &local);

  gboolean launched = FALSE;
  gboolean permitted = (local.action == BT_LINK_OPEN_ALLOW) ||
                       (confirmed_by_user &&
                        (local.action == BT_LINK_OPEN_CONFIRM_EXT ||
                         local.action == BT_LINK_OPEN_CONFIRM_DIR ||
                         local.action == BT_LINK_OPEN_CONFIRM_URL));

  if (permitted && launcher != NULL) {
    if (res->kind == BT_LINK_KIND_URL)
      launched = launcher(res->url, TRUE, user_data);
    else if (res->path != NULL)
      launched = launcher(res->path, FALSE, user_data);
  }

  if (out_policy != NULL)
    *out_policy = local;
  else
    bt_link_open_policy_clear(&local);
  return launched;
}

int bt_link_selftest_cli(const char *vault_dir)
{
  if (vault_dir == NULL || vault_dir[0] == '\0') {
    fprintf(stderr, "BT_SELFTEST_LINKS error: missing vault directory\n");
    return 1;
  }
  setlocale(LC_NUMERIC, "C");

  g_autoptr(GError) err = NULL;
  BtVault *vault = vault_open_readonly(vault_dir, NULL, &err);
  if (vault == NULL) {
    fprintf(stderr, "BT_SELFTEST_LINKS error: %s\n", err ? err->message : "cannot open vault");
    return 1;
  }

  gint64 t0 = g_get_monotonic_time();
  BtIndexSnapshot *snap = bt_index_snapshot_from_vault(vault);
  const GPtrArray *notes = vault_notes(vault);

  guint notes_with_links = 0;
  guint total_links = 0;
  guint by_syntax[4] = {0};
  guint by_status[6] = {0};
  guint by_kind[5] = {0};
  guint notes_with_math = 0;
  guint math_inline = 0;
  guint math_display = 0;

  GPtrArray *unresolved_lines = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *ambiguous_lines = g_ptr_array_new_with_free_func(g_free);

  for (guint ni = 0; ni < notes->len; ni++) {
    const gchar *rel = g_ptr_array_index(notes, ni);
    g_autofree gchar *raw = NULL;
    gsize rlen = 0;
    g_autoptr(GError) rerr = NULL;
    if (!vault_read(vault, rel, &raw, &rlen, &rerr)) {
      fprintf(stderr, "BT_SELFTEST_LINKS read error '%s': %s\n", rel, rerr ? rerr->message : "io");
      continue;
    }

    GArray *mspans = bt_math_scan(raw, rlen, NULL);
    if (mspans->len > 0)
      notes_with_math++;
    for (guint mi = 0; mi < mspans->len; mi++) {
      const BtMathSpan *ms = &g_array_index(mspans, BtMathSpan, mi);
      if (ms->mode == BT_MATH_DISPLAY)
        math_display++;
      else
        math_inline++;
    }
    g_array_unref(mspans);

    GArray *lspans = bt_link_scan(raw, rlen);
    if (lspans->len > 0)
      notes_with_links++;
    for (guint li = 0; li < lspans->len; li++) {
      const BtLinkSpan *sp = &g_array_index(lspans, BtLinkSpan, li);
      total_links++;
      if ((guint)sp->syntax < G_N_ELEMENTS(by_syntax))
        by_syntax[sp->syntax]++;

      BtLinkResolution *res = bt_link_resolve(snap, rel, sp->raw_target);
      if (res == NULL)
        continue;
      if ((guint)res->status < G_N_ELEMENTS(by_status))
        by_status[res->status]++;
      if ((guint)res->kind < G_N_ELEMENTS(by_kind))
        by_kind[res->kind]++;

      if (res->status == BT_LINK_UNRESOLVED && unresolved_lines->len < 50u) {
        g_ptr_array_add(unresolved_lines,
                        g_strdup_printf("%s:%u %s", rel, sp->line_no, sp->raw_target));
      } else if (res->status == BT_LINK_AMBIGUOUS) {
        GString *line = g_string_new(NULL);
        g_string_append_printf(line, "%s:%u %s ->", rel, sp->line_no, sp->raw_target);
        for (guint ci = 0; res->candidates != NULL && res->candidates[ci] != NULL; ci++) {
          g_string_append_printf(line, "%s %s", ci == 0 ? "" : ",", res->candidates[ci]);
        }
        g_ptr_array_add(ambiguous_lines, g_string_free(line, FALSE));
      }
      bt_link_resolution_free(res);
    }
    g_array_unref(lspans);
  }

  double total_ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
  double per_1k = (total_links > 0) ? (total_ms * 1000.0 / (double)total_links) : 0.0;

  printf("BT_SELFTEST_LINKS vault=\"%s\"\n", vault_root(vault));
  printf("notes=%u notes_with_links=%u total_links=%u total_ms=%.2f ms_per_1000_links=%.2f\n",
         notes->len, notes_with_links, total_links, total_ms, per_1k);
  printf("by_syntax: wikilink=%u markdown=%u bare_url=%u frontmatter=%u\n",
         by_syntax[BT_LINK_SYNTAX_WIKILINK], by_syntax[BT_LINK_SYNTAX_MARKDOWN],
         by_syntax[BT_LINK_SYNTAX_BARE_URL], by_syntax[BT_LINK_SYNTAX_FRONTMATTER]);
  printf("by_status: RESOLVED=%u AMBIGUOUS=%u UNRESOLVED=%u PLACEHOLDER=%u EXTERNAL=%u BLOCKED=%u\n",
         by_status[BT_LINK_RESOLVED], by_status[BT_LINK_AMBIGUOUS],
         by_status[BT_LINK_UNRESOLVED], by_status[BT_LINK_PLACEHOLDER],
         by_status[BT_LINK_EXTERNAL], by_status[BT_LINK_BLOCKED]);
  printf("by_kind: note=%u file=%u dir=%u url=%u same_note=%u\n",
         by_kind[BT_LINK_KIND_NOTE], by_kind[BT_LINK_KIND_FILE],
         by_kind[BT_LINK_KIND_DIR], by_kind[BT_LINK_KIND_URL],
         by_kind[BT_LINK_KIND_SAME_NOTE]);
  printf("math_tok: notes_with_math=%u inline=%u display=%u total=%u\n",
         notes_with_math, math_inline, math_display, math_inline + math_display);

  printf("--- UNRESOLVED (first %u of %u) ---\n",
         unresolved_lines->len, by_status[BT_LINK_UNRESOLVED]);
  for (guint i = 0; i < unresolved_lines->len; i++)
    printf("UNRESOLVED %s\n", (const char *)g_ptr_array_index(unresolved_lines, i));

  printf("--- AMBIGUOUS (%u) ---\n", ambiguous_lines->len);
  for (guint i = 0; i < ambiguous_lines->len; i++)
    printf("AMBIGUOUS %s\n", (const char *)g_ptr_array_index(ambiguous_lines, i));

  fflush(stdout);
  g_ptr_array_unref(ambiguous_lines);
  g_ptr_array_unref(unresolved_lines);
  bt_index_snapshot_free(snap);
  vault_free(vault);
  return 0;
}
