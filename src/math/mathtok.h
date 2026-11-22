/* mathtok.h — GTK-free math delimiter tokenizer (Phase M1, §B.2).
 *
 * Scans UTF-8 Markdown buffers for math spans ($...$, $$...$$, \(...\), \[...\],
 * and top-level math environments) BEFORE Markdown emphasis or link rules run.
 * Safe on untrusted inputs: bounded reads (never past len), CRLF/BOM aware,
 * 200-line / 16 KB display cap, 4 KB inline cap. */
#ifndef BT_MATHTOK_H
#define BT_MATHTOK_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BT_MATH_INLINE = 0,
  BT_MATH_DISPLAY = 1,
} BtMathMode;

typedef struct {
  gsize start;       /* byte offset of opening delimiter */
  gsize end;         /* byte offset just past closing delimiter */
  gsize src_start;   /* byte offset of inner LaTeX source */
  gsize src_end;     /* byte offset just past inner LaTeX source */
  BtMathMode mode;
  guint32 hash;      /* FNV-1a 32-bit hash of (mode + normalized source) */
  guint line_first;  /* 1-based start line */
  guint line_last;   /* 1-based end line */
} BtMathSpan;

typedef struct {
  gboolean allow_paren_bracket; /* \(...\) and \[...\] */
  gboolean allow_environments;  /* \begin{equation|align|gather[*]} */
  gboolean skip_frontmatter;    /* ignore top YAML --- ... --- */
} BtMathScanOpts;

/* Default options: all three flags TRUE. */
BtMathScanOpts bt_math_scan_default_opts(void);

/* Scans text[0..len) and returns a GArray of sorted, non-overlapping BtMathSpan.
 * Caller owns the returned GArray (g_array_unref). */
GArray *bt_math_scan(const char *text, gsize len, const BtMathScanOpts *opts);

/* Scans only the enclosing paragraph(s)/block covering [range_start, range_end)
 * and sets *out_block_start / *out_block_end to the byte region rescanned.
 * Returned spans have buffer-relative byte offsets within [out_block_start, out_block_end]. */
GArray *bt_math_scan_range(const char *text, gsize len, gsize range_start, gsize range_end,
                           const BtMathScanOpts *opts,
                           gsize *out_block_start, gsize *out_block_end);

/* Binary search: returns TRUE if byte_offset lies inside a span [start, end). */
gboolean bt_math_offset_in_spans(const GArray *spans, gsize byte_offset, BtMathSpan *out_span);

/* Returns runtime HarfBuzz version string (proves HarfBuzz linkage). */
const char *bt_math_harfbuzz_version(void);

G_END_DECLS

#endif
