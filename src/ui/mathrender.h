/* mathrender.h — Cairo-FT math renderer & surface cache (M4). */
#ifndef BT_MATHRENDER_H
#define BT_MATHRENDER_H

#include <cairo.h>
#include <glib.h>

#include "math/mathfont.h"
#include "math/mathlayout.h"

G_BEGIN_DECLS

typedef struct BtMathRenderer BtMathRenderer;

typedef struct {
  cairo_surface_t *surface; /* Image surface rendered at device_scale */
  double width_px;          /* Logical width in CSS/widget pixels */
  double height_px;         /* Logical height in CSS/widget pixels */
  double baseline_y_px;     /* Logical baseline Y from top of surface */
  double ascent_px;         /* Logical ascent above baseline */
  double descent_px;        /* Logical descent below baseline */
  guint error_count;        /* Number of parse errors in formula */
} BtMathRendered;

BtMathRenderer *bt_math_renderer_get_default(void);
BtMathRenderer *bt_math_renderer_new(BtMathFont *font);
void bt_math_renderer_free(BtMathRenderer *r);

/* Draws a BtMathLayout directly onto a Cairo context at (origin_x, baseline_y)
 * with base font size font_px and foreground color (fg_r, fg_g, fg_b, fg_a). */
void bt_math_renderer_draw_layout(BtMathRenderer *r, cairo_t *cr,
                                  const BtMathLayout *layout,
                                  double origin_x, double baseline_y,
                                  double font_px,
                                  double fg_r, double fg_g, double fg_b, double fg_a);

/* Renders (or fetches from the 512-entry LRU cache) a compiled math surface for
 * raw LaTeX string `latex` (without $ or $$ delimiters).
 * The returned BtMathRendered pointer is owned by the renderer cache and valid
 * until evicted or renderer destruction. */
const BtMathRendered *bt_math_renderer_get_cached(BtMathRenderer *r,
                                                  const char *latex, gsize len,
                                                  BtMathMode mode,
                                                  double font_px, int device_scale);

void bt_math_renderer_clear_cache(BtMathRenderer *r);

G_END_DECLS

#endif /* BT_MATHRENDER_H */
