/* noteutil.c — see noteutil.h. */
#include "noteutil.h"

#include <string.h>

static gboolean is_boundary(gchar c)
{
  return c == '/' || c == '-' || c == '_' || c == ' ' || c == '.';
}

static gint score_subseq(const gchar *query, const gchar *str, const gchar *base)
{
  gint score = 0;
  gint run = 0;
  const gchar *c = str;
  for (const gchar *q = query; *q != '\0'; q++) {
    gchar qc = g_ascii_tolower(*q);
    if (qc == ' ')
      continue;
    gboolean found = FALSE;
    while (*c != '\0') {
      gchar cc = g_ascii_tolower(*c);
      if (cc == qc) {
        score += 1;
        if (run > 0)
          score += 6;
        if (c == str || is_boundary(c[-1]))
          score += 5;
        if (c >= base)
          score += 3;
        run++;
        c++;
        found = TRUE;
        break;
      }
      run = 0;
      c++;
    }
    if (!found)
      return -1;
  }
  return MAX(score, 1);
}

gint bt_fuzzy_score(const gchar *query, const gchar *candidate)
{
  if (query == NULL || candidate == NULL)
    return -1;
  if (query[0] == '\0')
    return 0;
  const gchar *base = strrchr(candidate, '/');
  base = base ? base + 1 : candidate;

  gint full = score_subseq(query, candidate, base);
  gint base_score = score_subseq(query, base, base);
  gint best = MAX(full, base_score);
  if (best < 0)
    return -1;
  best -= (gint)(strlen(candidate) / 16);
  return MAX(best, 1);
}

gchar *bt_wikilink_at(const gchar *line, gsize byte_offset)
{
  if (line == NULL)
    return NULL;
  const gchar *p = line;
  for (;;) {
    const gchar *open = strstr(p, "[[");
    if (open == NULL)
      return NULL;
    const gchar *close = strstr(open + 2, "]]");
    if (close == NULL)
      return NULL;
    gsize start = (gsize)(open - line);
    gsize end = (gsize)(close - line) + 2;
    if (byte_offset >= start && byte_offset < end) {
      gsize inner_len = (gsize)(close - (open + 2));
      gchar *inner = g_strndup(open + 2, inner_len);
      gchar *cut = strpbrk(inner, "|#");
      if (cut != NULL)
        *cut = '\0';
      g_strstrip(inner);
      if (inner[0] == '\0' || strstr(inner, "[[") != NULL) {
        g_free(inner);
        return NULL;
      }
      return inner;
    }
    p = close + 2;
  }
}

static gchar *strip_md(const gchar *s)
{
  gsize n = strlen(s);
  if (n > 3 && g_ascii_strcasecmp(s + n - 3, ".md") == 0)
    return g_strndup(s, n - 3);
  return g_strdup(s);
}

static gboolean utf8_ci_equal(const gchar *a, const gchar *b)
{
  gchar *fa = g_utf8_casefold(a, -1);
  gchar *fb = g_utf8_casefold(b, -1);
  gboolean eq = strcmp(fa, fb) == 0;
  g_free(fa);
  g_free(fb);
  return eq;
}

gchar *bt_wikilink_find_note(const GPtrArray *notes, const gchar *target)
{
  if (notes == NULL || target == NULL || !g_utf8_validate(target, -1, NULL))
    return NULL;
  gchar *want = strip_md(target);
  for (gchar *p = want; *p != '\0'; p++)
    if (*p == '\\')
      *p = '/';
  const gchar *by_base = NULL;
  for (guint i = 0; i < notes->len; i++) {
    const gchar *rel = g_ptr_array_index(notes, i);
    gchar *stem = strip_md(rel);
    if (utf8_ci_equal(stem, want)) {
      g_free(stem);
      g_free(want);
      return g_strdup(rel);
    }
    if (by_base == NULL) {
      const gchar *b = strrchr(stem, '/');
      if (utf8_ci_equal(b ? b + 1 : stem, want))
        by_base = rel;
    }
    g_free(stem);
  }
  g_free(want);
  return by_base ? g_strdup(by_base) : NULL;
}

gchar *bt_wikilink_new_note_path(const gchar *target)
{
  if (target == NULL || target[0] == '\0')
    return NULL;
  gchar *rel = g_strdup(target);
  for (gchar *p = rel; *p != '\0'; p++)
    if (*p == '\\')
      *p = '/';
  gsize n = strlen(rel);
  if (n > 3 && g_ascii_strcasecmp(rel + n - 3, ".md") == 0)
    return rel;
  gchar *out = g_strconcat(rel, ".md", NULL);
  g_free(rel);
  return out;
}
