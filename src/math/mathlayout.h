/* mathlayout.h — OpenType MATH & TeX box layout engine (M3).
 * Pure C17, GLib + HarfBuzz only (no GTK, no Cairo).
 * Coordinate convention:
 *   - All dimensions are in em units of the base font (1.0 = base font_px).
 *   - X grows to the right from 0.0.
 *   - Y is relative to the math baseline (y = 0.0 is the baseline;
 *     y < 0.0 is ABOVE the baseline; y > 0.0 is BELOW the baseline,
 *     matching Cairo's Y-down screen coordinate system directly). */
#ifndef BT_MATHLAYOUT_H
#define BT_MATHLAYOUT_H

#include <glib.h>
#include "mathfont.h"
#include "mathparse.h"

G_BEGIN_DECLS

typedef enum {
  BT_STYLE_DISPLAY       = 0, /* D  (scale 1.0) */
  BT_STYLE_TEXT          = 1, /* T  (scale 1.0) */
  BT_STYLE_SCRIPT        = 2, /* S  (scale ~0.70) */
  BT_STYLE_SCRIPTSCRIPT  = 3, /* SS (scale ~0.55) */
} BtMathStyle;

typedef enum {
  BT_DRAW_GLYPH = 0, /* Positioned glyph in STIXTwoMath-Regular.otf */
  BT_DRAW_RULE  = 1, /* Filled rectangle (fraction bar, radical vinculum, overline, error box) */
} BtDrawOpKind;

typedef struct {
  BtDrawOpKind kind;
  gboolean is_error;
  union {
    struct {
      hb_codepoint_t glyph_id;
      gunichar unicode;
      double x_em;     /* Glyph origin X in em */
      double y_em;     /* Glyph baseline Y in em (0 = formula baseline, < 0 above, > 0 below) */
      double scale;    /* Font scale multiplier relative to base font_px (e.g. 1.0, 0.70, 0.55) */
    } glyph;
    struct {
      double x_em;     /* Top-left X in em */
      double y_em;     /* Top-left Y in em (< 0 above baseline) */
      double w_em;     /* Width in em */
      double h_em;     /* Thickness/height in em */
    } rule;
  } u;
} BtDrawOp;

typedef struct {
  double width_em;     /* Total advance width >= 0 */
  double ascent_em;    /* Extent above baseline >= 0 (top edge is at y = -ascent_em) */
  double descent_em;   /* Extent below baseline >= 0 (bottom edge is at y = +descent_em) */
  BtMathMode mode;
  GArray *ops;         /* Array of BtDrawOp */
} BtMathLayout;

/* Lays out a parsed AST using the OpenType MATH font table metrics into an
 * immutable draw list in em units. Never returns NULL if font and ast are non-NULL. */
BtMathLayout *bt_math_layout_build(const BtMathFont *font, const BtMathParseResult *ast);

/* Convenience helper: parses src and builds layout in one step. */
BtMathLayout *bt_math_layout_from_latex(const BtMathFont *font, const char *src, gsize len,
                                        BtMathMode mode);

void bt_math_layout_free(BtMathLayout *layout);

G_END_DECLS

#endif /* BT_MATHLAYOUT_H */
