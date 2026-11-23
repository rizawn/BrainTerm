/* mathfont.h — OpenType MATH font table reader via HarfBuzz <hb-ot.h> (M3).
 * Pure C17, GLib + HarfBuzz only (no GTK, no Cairo). */
#ifndef BT_MATHFONT_H
#define BT_MATHFONT_H

#include <glib.h>
#include <hb.h>
#include <hb-ot.h>

G_BEGIN_DECLS

typedef struct BtMathFont BtMathFont;

typedef struct {
  double advance_em;
  double ascent_em;   /* Height above baseline (>= 0 for normal glyphs) */
  double descent_em;  /* Depth below baseline (>= 0 for descenders) */
  double left_bearing_em;
  double right_bearing_em;
  double italic_corr_em;
  double top_accent_x_em;
} BtGlyphMetrics;

typedef struct {
  double axis_height;
  double script_scale;
  double script_script_scale;

  /* Fractions */
  double frac_rule_thickness;
  double frac_num_shift_up;
  double frac_num_disp_shift_up;
  double frac_den_shift_down;
  double frac_den_disp_shift_down;
  double frac_num_gap_min;
  double frac_num_disp_gap_min;
  double frac_den_gap_min;
  double frac_den_disp_gap_min;

  /* Stack / binomial */
  double stack_top_shift_up;
  double stack_top_disp_shift_up;
  double stack_bot_shift_down;
  double stack_bot_disp_shift_down;
  double stack_gap_min;
  double stack_disp_gap_min;

  /* Scripts */
  double sup_shift_up;
  double sup_shift_up_cramped;
  double sub_shift_down;
  double sup_bottom_min;
  double sub_top_max;
  double sub_sup_gap_min;
  double sup_bottom_max_with_sub;
  double space_after_script;

  /* Radicals */
  double radical_extra_ascender;
  double radical_rule_thickness;
  double radical_vgap;
  double radical_disp_vgap;
  double radical_kern_before_deg;
  double radical_kern_after_deg;
  double radical_deg_bottom_raise_pct;

  /* Limits */
  double upper_limit_gap_min;
  double upper_limit_baseline_rise_min;
  double lower_limit_gap_min;
  double lower_limit_baseline_drop_min;

  /* Accents & over/underbars */
  double accent_base_height;
  double overbar_vgap;
  double overbar_rule_thickness;
  double overbar_extra_ascender;
  double underbar_vgap;
  double underbar_rule_thickness;
  double underbar_extra_descender;
} BtMathConstants;

/* Loads the math font from raw OTF/TTF bytes in memory (takes a reference or copy). */
BtMathFont *bt_math_font_new_from_bytes(GBytes *font_bytes);

/* Loads the math font from a file path on disk. */
BtMathFont *bt_math_font_new_from_file(const char *path);

/* Loads the default bundled STIXTwoMath-Regular.otf (searching data/fonts, share/brainterm/fonts,
 * GResource /local/brainterm/fonts/STIXTwoMath-Regular.otf, or Windows Cambria Math fallback). */
BtMathFont *bt_math_font_get_default(void);

void bt_math_font_free(BtMathFont *font);

GBytes *bt_math_font_get_bytes(const BtMathFont *font);
hb_font_t *bt_math_font_get_hb_font(const BtMathFont *font);
unsigned int bt_math_font_get_upem(const BtMathFont *font);
const BtMathConstants *bt_math_font_get_constants(const BtMathFont *font);

/* Maps a Unicode codepoint to a glyph ID in the math font (with automatic fallback
 * from Mathematical Alphanumeric Symbols to base ASCII/Greek if a codepoint is missing). */
hb_codepoint_t bt_math_font_get_glyph(const BtMathFont *font, gunichar unicode);

/* Returns glyph metrics in em units (1.0 = upem) at scale = 1.0. */
void bt_math_font_get_glyph_metrics(const BtMathFont *font, hb_codepoint_t glyph,
                                    BtGlyphMetrics *out_metrics);

/* Finds a vertical size variant of base_glyph whose total height (ascent + descent)
 * at the given scale is >= target_height_em (or the largest available variant). */
hb_codepoint_t bt_math_font_get_v_variant(const BtMathFont *font, hb_codepoint_t base_glyph,
                                          double target_height_em, double scale);

/* Finds a horizontal size variant of base_glyph whose advance width at scale is >= target_width_em. */
hb_codepoint_t bt_math_font_get_h_variant(const BtMathFont *font, hb_codepoint_t base_glyph,
                                          double target_width_em, double scale);

G_END_DECLS

#endif /* BT_MATHFONT_H */
