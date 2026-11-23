/* mathfont.c — OpenType MATH font table reader via HarfBuzz <hb-ot.h> (M3). */
#include "mathfont.h"

#include <gio/gio.h>
#include <string.h>

struct BtMathFont {
  GBytes *bytes;
  hb_blob_t *blob;
  hb_face_t *face;
  hb_font_t *font;
  unsigned int upem;
  BtMathConstants c;
};

static double em_from_hb(hb_position_t v, unsigned int upem)
{
  return (double)v / (double)upem;
}

static double get_const_em(hb_font_t *font, hb_ot_math_constant_t k, unsigned int upem, double fallback)
{
  if (!hb_ot_math_has_data(hb_font_get_face(font)))
    return fallback;
  hb_position_t v = hb_ot_math_get_constant(font, k);
  return em_from_hb(v, upem);
}

static void populate_constants(BtMathFont *mf)
{
  hb_font_t *f = mf->font;
  unsigned int u = mf->upem;
  gboolean has_math = hb_ot_math_has_data(mf->face);

  if (has_math) {
    hb_position_t sp = hb_ot_math_get_constant(f, HB_OT_MATH_CONSTANT_SCRIPT_PERCENT_SCALE_DOWN);
    hb_position_t ssp = hb_ot_math_get_constant(f, HB_OT_MATH_CONSTANT_SCRIPT_SCRIPT_PERCENT_SCALE_DOWN);
    mf->c.script_scale = (sp > 0) ? ((double)sp / 100.0) : 0.70;
    mf->c.script_script_scale = (ssp > 0) ? ((double)ssp / 100.0) : 0.55;
  } else {
    mf->c.script_scale = 0.70;
    mf->c.script_script_scale = 0.55;
  }

  mf->c.axis_height = get_const_em(f, HB_OT_MATH_CONSTANT_AXIS_HEIGHT, u, 0.25);

  mf->c.frac_rule_thickness = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_RULE_THICKNESS, u, 0.045), 0.035);
  mf->c.frac_num_shift_up = get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_NUMERATOR_SHIFT_UP, u, 0.42);
  mf->c.frac_num_disp_shift_up = get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_NUMERATOR_DISPLAY_STYLE_SHIFT_UP, u, 0.68);
  mf->c.frac_den_shift_down = get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_DENOMINATOR_SHIFT_DOWN, u, 0.42);
  mf->c.frac_den_disp_shift_down = get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_DENOMINATOR_DISPLAY_STYLE_SHIFT_DOWN, u, 0.68);
  mf->c.frac_num_gap_min = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_NUMERATOR_GAP_MIN, u, 0.06), 0.04);
  mf->c.frac_num_disp_gap_min = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_NUM_DISPLAY_STYLE_GAP_MIN, u, 0.14), 0.10);
  mf->c.frac_den_gap_min = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_DENOMINATOR_GAP_MIN, u, 0.06), 0.04);
  mf->c.frac_den_disp_gap_min = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_FRACTION_DENOM_DISPLAY_STYLE_GAP_MIN, u, 0.14), 0.10);

  mf->c.stack_top_shift_up = get_const_em(f, HB_OT_MATH_CONSTANT_STACK_TOP_SHIFT_UP, u, 0.45);
  mf->c.stack_top_disp_shift_up = get_const_em(f, HB_OT_MATH_CONSTANT_STACK_TOP_DISPLAY_STYLE_SHIFT_UP, u, 0.70);
  mf->c.stack_bot_shift_down = get_const_em(f, HB_OT_MATH_CONSTANT_STACK_BOTTOM_SHIFT_DOWN, u, 0.38);
  mf->c.stack_bot_disp_shift_down = get_const_em(f, HB_OT_MATH_CONSTANT_STACK_BOTTOM_DISPLAY_STYLE_SHIFT_DOWN, u, 0.65);
  mf->c.stack_gap_min = get_const_em(f, HB_OT_MATH_CONSTANT_STACK_GAP_MIN, u, 0.15);
  mf->c.stack_disp_gap_min = get_const_em(f, HB_OT_MATH_CONSTANT_STACK_DISPLAY_STYLE_GAP_MIN, u, 0.30);

  mf->c.sup_shift_up = get_const_em(f, HB_OT_MATH_CONSTANT_SUPERSCRIPT_SHIFT_UP, u, 0.36);
  mf->c.sup_shift_up_cramped = get_const_em(f, HB_OT_MATH_CONSTANT_SUPERSCRIPT_SHIFT_UP_CRAMPED, u, 0.29);
  mf->c.sub_shift_down = get_const_em(f, HB_OT_MATH_CONSTANT_SUBSCRIPT_SHIFT_DOWN, u, 0.20);
  mf->c.sup_bottom_min = get_const_em(f, HB_OT_MATH_CONSTANT_SUPERSCRIPT_BOTTOM_MIN, u, 0.12);
  mf->c.sub_top_max = get_const_em(f, HB_OT_MATH_CONSTANT_SUBSCRIPT_TOP_MAX, u, 0.36);
  mf->c.sub_sup_gap_min = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_SUB_SUPERSCRIPT_GAP_MIN, u, 0.15), 0.10);
  mf->c.sup_bottom_max_with_sub = get_const_em(f, HB_OT_MATH_CONSTANT_SUPERSCRIPT_BOTTOM_MAX_WITH_SUBSCRIPT, u, 0.36);
  mf->c.space_after_script = get_const_em(f, HB_OT_MATH_CONSTANT_SPACE_AFTER_SCRIPT, u, 0.04);

  mf->c.radical_extra_ascender = get_const_em(f, HB_OT_MATH_CONSTANT_RADICAL_EXTRA_ASCENDER, u, 0.05);
  mf->c.radical_rule_thickness = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_RADICAL_RULE_THICKNESS, u, 0.045), 0.035);
  mf->c.radical_vgap = get_const_em(f, HB_OT_MATH_CONSTANT_RADICAL_VERTICAL_GAP, u, 0.06);
  mf->c.radical_disp_vgap = get_const_em(f, HB_OT_MATH_CONSTANT_RADICAL_DISPLAY_STYLE_VERTICAL_GAP, u, 0.12);
  mf->c.radical_kern_before_deg = get_const_em(f, HB_OT_MATH_CONSTANT_RADICAL_KERN_BEFORE_DEGREE, u, 0.10);
  mf->c.radical_kern_after_deg = get_const_em(f, HB_OT_MATH_CONSTANT_RADICAL_KERN_AFTER_DEGREE, u, -0.35);
  hb_position_t rpct = has_math ? hb_ot_math_get_constant(f, HB_OT_MATH_CONSTANT_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT) : 60;
  mf->c.radical_deg_bottom_raise_pct = (rpct > 0) ? ((double)rpct / 100.0) : 0.60;

  mf->c.upper_limit_gap_min = get_const_em(f, HB_OT_MATH_CONSTANT_UPPER_LIMIT_GAP_MIN, u, 0.10);
  mf->c.upper_limit_baseline_rise_min = get_const_em(f, HB_OT_MATH_CONSTANT_UPPER_LIMIT_BASELINE_RISE_MIN, u, 0.20);
  mf->c.lower_limit_gap_min = get_const_em(f, HB_OT_MATH_CONSTANT_LOWER_LIMIT_GAP_MIN, u, 0.10);
  mf->c.lower_limit_baseline_drop_min = get_const_em(f, HB_OT_MATH_CONSTANT_LOWER_LIMIT_BASELINE_DROP_MIN, u, 0.35);

  mf->c.accent_base_height = get_const_em(f, HB_OT_MATH_CONSTANT_ACCENT_BASE_HEIGHT, u, 0.48);
  mf->c.overbar_vgap = get_const_em(f, HB_OT_MATH_CONSTANT_OVERBAR_VERTICAL_GAP, u, 0.12);
  mf->c.overbar_rule_thickness = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_OVERBAR_RULE_THICKNESS, u, 0.045), 0.035);
  mf->c.overbar_extra_ascender = get_const_em(f, HB_OT_MATH_CONSTANT_OVERBAR_EXTRA_ASCENDER, u, 0.04);
  mf->c.underbar_vgap = get_const_em(f, HB_OT_MATH_CONSTANT_UNDERBAR_VERTICAL_GAP, u, 0.12);
  mf->c.underbar_rule_thickness = MAX(get_const_em(f, HB_OT_MATH_CONSTANT_UNDERBAR_RULE_THICKNESS, u, 0.045), 0.035);
  mf->c.underbar_extra_descender = get_const_em(f, HB_OT_MATH_CONSTANT_UNDERBAR_EXTRA_DESCENDER, u, 0.04);
}

BtMathFont *bt_math_font_new_from_bytes(GBytes *font_bytes)
{
  if (font_bytes == NULL)
    return NULL;
  gsize size = 0;
  const char *data = g_bytes_get_data(font_bytes, &size);
  if (data == NULL || size == 0)
    return NULL;

  GBytes *ref = g_bytes_ref(font_bytes);
  hb_blob_t *blob = hb_blob_create(data, (unsigned int)size, HB_MEMORY_MODE_READONLY,
                                   ref, (hb_destroy_func_t)g_bytes_unref);
  hb_face_t *face = hb_face_create(blob, 0);
  unsigned int upem = hb_face_get_upem(face);
  if (upem == 0)
    upem = 1000;
  hb_font_t *font = hb_font_create(face);
  hb_font_set_scale(font, (int)upem, (int)upem);
  hb_ot_font_set_funcs(font);

  BtMathFont *mf = g_new0(BtMathFont, 1);
  mf->bytes = g_bytes_ref(font_bytes);
  mf->blob = blob;
  mf->face = face;
  mf->font = font;
  mf->upem = upem;
  populate_constants(mf);
  return mf;
}

BtMathFont *bt_math_font_new_from_file(const char *path)
{
  if (path == NULL)
    return NULL;
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &contents, &len, NULL))
    return NULL;
  GBytes *bytes = g_bytes_new_take(contents, len);
  BtMathFont *mf = bt_math_font_new_from_bytes(bytes);
  g_bytes_unref(bytes);
  return mf;
}

BtMathFont *bt_math_font_get_default(void)
{
  static BtMathFont *shared = NULL;
  if (shared != NULL)
    return shared;

  /* 1. Try GResource /local/brainterm/fonts/STIXTwoMath-Regular.otf */
  GBytes *res_bytes = g_resources_lookup_data("/local/brainterm/fonts/STIXTwoMath-Regular.otf",
                                              G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (res_bytes != NULL) {
    shared = bt_math_font_new_from_bytes(res_bytes);
    g_bytes_unref(res_bytes);
    if (shared != NULL)
      return shared;
  }

  /* 2. Search BT_SOURCE_ROOT, cwd, and exe-relative directories */
  g_autoptr(GPtrArray) cands = g_ptr_array_new_with_free_func(g_free);
  const char *src_root = g_getenv("BT_SOURCE_ROOT");
  if (src_root != NULL && src_root[0] != '\0')
    g_ptr_array_add(cands, g_build_filename(src_root, "data", "fonts", "STIXTwoMath-Regular.otf", NULL));

  g_autofree gchar *cwd = g_get_current_dir();
  g_ptr_array_add(cands, g_build_filename(cwd, "data", "fonts", "STIXTwoMath-Regular.otf", NULL));
  g_ptr_array_add(cands, g_build_filename(cwd, "..", "data", "fonts", "STIXTwoMath-Regular.otf", NULL));
  g_ptr_array_add(cands, g_build_filename(cwd, "share", "brainterm", "fonts", "STIXTwoMath-Regular.otf", NULL));

#ifdef G_OS_WIN32
  g_autofree gchar *exe_dir = g_win32_get_package_installation_directory_of_module(NULL);
  if (exe_dir != NULL) {
    g_ptr_array_add(cands, g_build_filename(exe_dir, "share", "brainterm", "fonts", "STIXTwoMath-Regular.otf", NULL));
    g_ptr_array_add(cands, g_build_filename(exe_dir, "..", "share", "brainterm", "fonts", "STIXTwoMath-Regular.otf", NULL));
    g_ptr_array_add(cands, g_build_filename(exe_dir, "..", "..", "data", "fonts", "STIXTwoMath-Regular.otf", NULL));
  }
#endif

  for (guint i = 0; i < cands->len; i++) {
    const char *p = g_ptr_array_index(cands, i);
    if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
      shared = bt_math_font_new_from_file(p);
      if (shared != NULL)
        return shared;
    }
  }

  return NULL;
}

void bt_math_font_free(BtMathFont *font)
{
  if (font == NULL)
    return;
  if (font->font != NULL)
    hb_font_destroy(font->font);
  if (font->face != NULL)
    hb_face_destroy(font->face);
  if (font->blob != NULL)
    hb_blob_destroy(font->blob);
  if (font->bytes != NULL)
    g_bytes_unref(font->bytes);
  g_free(font);
}

GBytes *bt_math_font_get_bytes(const BtMathFont *font)
{
  return font ? font->bytes : NULL;
}

hb_font_t *bt_math_font_get_hb_font(const BtMathFont *font)
{
  return font ? font->font : NULL;
}

unsigned int bt_math_font_get_upem(const BtMathFont *font)
{
  return font ? font->upem : 1000;
}

const BtMathConstants *bt_math_font_get_constants(const BtMathFont *font)
{
  return font ? &font->c : NULL;
}

static gunichar demote_math_alnum(gunichar cp)
{
  /* Mathematical Bold A-Z / a-z */
  if (cp >= 0x1D400u && cp <= 0x1D419u) return 'A' + (cp - 0x1D400u);
  if (cp >= 0x1D41Au && cp <= 0x1D433u) return 'a' + (cp - 0x1D41Au);
  /* Mathematical Italic A-Z / a-z */
  if (cp >= 0x1D434u && cp <= 0x1D44Du) return 'A' + (cp - 0x1D434u);
  if (cp >= 0x1D44Eu && cp <= 0x1D467u) return 'a' + (cp - 0x1D44Eu);
  if (cp == 0x210Eu) return 'h';
  /* Mathematical Italic Greek lowercase U+1D6FC..U+1D714 -> U+03B1..U+03C9 */
  if (cp >= 0x1D6FCu && cp <= 0x1D714u) return 0x03B1u + (cp - 0x1D6FCu);
  return 0;
}

hb_codepoint_t bt_math_font_get_glyph(const BtMathFont *font, gunichar unicode)
{
  if (font == NULL || font->font == NULL || unicode == 0)
    return 0;
  hb_codepoint_t gid = 0;
  if (hb_font_get_nominal_glyph(font->font, unicode, &gid) && gid != 0)
    return gid;
  gunichar base = demote_math_alnum(unicode);
  if (base != 0 && hb_font_get_nominal_glyph(font->font, base, &gid) && gid != 0)
    return gid;
  hb_font_get_nominal_glyph(font->font, '?', &gid);
  return gid;
}

void bt_math_font_get_glyph_metrics(const BtMathFont *font, hb_codepoint_t glyph,
                                    BtGlyphMetrics *out_metrics)
{
  memset(out_metrics, 0, sizeof(*out_metrics));
  if (font == NULL || font->font == NULL || glyph == 0)
    return;

  double u = (double)font->upem;
  hb_position_t adv = hb_font_get_glyph_h_advance(font->font, glyph);
  out_metrics->advance_em = (double)adv / u;

  hb_glyph_extents_t ext = {0};
  if (hb_font_get_glyph_extents(font->font, glyph, &ext)) {
    /* In HarfBuzz Y-up coordinates:
     * y_bearing is top of glyph above baseline (> 0 above baseline).
     * height is negative (< 0 going downward).
     * So bottom of glyph = y_bearing + height. */
    double top = (double)ext.y_bearing / u;
    double bottom = (double)(ext.y_bearing + ext.height) / u;
    out_metrics->ascent_em = MAX(top, 0.0);
    out_metrics->descent_em = MAX(-bottom, 0.0);
    out_metrics->left_bearing_em = (double)ext.x_bearing / u;
    out_metrics->right_bearing_em = (double)(ext.x_bearing + ext.width) / u;
  } else {
    out_metrics->ascent_em = 0.65;
    out_metrics->descent_em = 0.15;
  }

  if (hb_ot_math_has_data(font->face)) {
    out_metrics->italic_corr_em =
        (double)hb_ot_math_get_glyph_italics_correction(font->font, glyph) / u;
    hb_position_t acc = hb_ot_math_get_glyph_top_accent_attachment(font->font, glyph);
    if (acc != 0)
      out_metrics->top_accent_x_em = (double)acc / u;
    else
      out_metrics->top_accent_x_em = out_metrics->advance_em * 0.5;
  } else {
    out_metrics->top_accent_x_em = out_metrics->advance_em * 0.5;
  }
}

hb_codepoint_t bt_math_font_get_v_variant(const BtMathFont *font, hb_codepoint_t base_glyph,
                                          double target_height_em, double scale)
{
  if (font == NULL || font->font == NULL || base_glyph == 0)
    return base_glyph;
  if (!hb_ot_math_has_data(font->face))
    return base_glyph;

  hb_ot_math_glyph_variant_t vars[32];
  unsigned int count = G_N_ELEMENTS(vars);
  unsigned int total = hb_ot_math_get_glyph_variants(font->font, base_glyph,
                                                     HB_DIRECTION_TTB, 0,
                                                     &count, vars);
  if (total == 0 || count == 0)
    return base_glyph;

  hb_codepoint_t best = base_glyph;
  for (unsigned int i = 0; i < count; i++) {
    best = vars[i].glyph;
    BtGlyphMetrics gm;
    bt_math_font_get_glyph_metrics(font, best, &gm);
    double h = (gm.ascent_em + gm.descent_em) * scale;
    if (h >= target_height_em * 0.96)
      return best;
  }
  return best;
}

hb_codepoint_t bt_math_font_get_h_variant(const BtMathFont *font, hb_codepoint_t base_glyph,
                                          double target_width_em, double scale)
{
  if (font == NULL || font->font == NULL || base_glyph == 0)
    return base_glyph;
  if (!hb_ot_math_has_data(font->face))
    return base_glyph;

  hb_ot_math_glyph_variant_t vars[32];
  unsigned int count = G_N_ELEMENTS(vars);
  unsigned int total = hb_ot_math_get_glyph_variants(font->font, base_glyph,
                                                     HB_DIRECTION_LTR, 0,
                                                     &count, vars);
  if (total == 0 || count == 0)
    return base_glyph;

  hb_codepoint_t best = base_glyph;
  for (unsigned int i = 0; i < count; i++) {
    best = vars[i].glyph;
    BtGlyphMetrics gm;
    bt_math_font_get_glyph_metrics(font, best, &gm);
    if (gm.advance_em * scale >= target_width_em * 0.95)
      return best;
  }
  return best;
}
