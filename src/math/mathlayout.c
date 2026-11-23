/* mathlayout.c — OpenType MATH & TeX box layout engine (M3). */
#include "mathlayout.h"

#include <math.h>
#include <string.h>

typedef struct {
  double width;
  double ascent;
  double descent;
  double italic_corr;
  double top_accent_x;
  BtAtomClass atom_class;
  hb_codepoint_t single_glyph;
  GArray *ops; /* Array of BtDrawOp */
} MathBox;

static void box_init(MathBox *b, BtAtomClass cls)
{
  memset(b, 0, sizeof(*b));
  b->atom_class = cls;
  b->ops = g_array_new(FALSE, FALSE, sizeof(BtDrawOp));
}

static void box_clear(MathBox *b)
{
  if (b->ops != NULL) {
    g_array_unref(b->ops);
    b->ops = NULL;
  }
}

/* Appends all draw ops from child into parent shifted by (dx_em, dy_em).
 * Remember: Y is Cairo Y-down (baseline = 0; above baseline is dy < 0; below is dy > 0). */
static void box_merge_at(MathBox *parent, const MathBox *child, double dx, double dy)
{
  for (guint i = 0; i < child->ops->len; i++) {
    BtDrawOp op = g_array_index(child->ops, BtDrawOp, i);
    if (op.kind == BT_DRAW_GLYPH) {
      op.u.glyph.x_em += dx;
      op.u.glyph.y_em += dy;
    } else {
      op.u.rule.x_em += dx;
      op.u.rule.y_em += dy;
    }
    g_array_append_val(parent->ops, op);
  }
  /* Child top edge in parent coords is (-child->ascent + dy), so parent ascent is max(ascent, child->ascent - dy).
   * Child bottom edge in parent coords is (child->descent + dy), so parent descent is max(descent, child->descent + dy). */
  if (child->width > 0.0 || child->ascent > 0.0 || child->descent > 0.0 || child->ops->len > 0) {
    parent->ascent = MAX(parent->ascent, child->ascent - dy);
    parent->descent = MAX(parent->descent, child->descent + dy);
    parent->width = MAX(parent->width, dx + child->width);
  }
}

static void box_add_glyph(MathBox *b, hb_codepoint_t gid, gunichar uc,
                          double x, double y, double scale, gboolean is_err)
{
  BtDrawOp op;
  memset(&op, 0, sizeof(op));
  op.kind = BT_DRAW_GLYPH;
  op.is_error = is_err;
  op.u.glyph.glyph_id = gid;
  op.u.glyph.unicode = uc;
  op.u.glyph.x_em = x;
  op.u.glyph.y_em = y;
  op.u.glyph.scale = scale;
  g_array_append_val(b->ops, op);
}

static void box_add_rule(MathBox *b, double x, double y, double w, double h, gboolean is_err)
{
  BtDrawOp op;
  memset(&op, 0, sizeof(op));
  op.kind = BT_DRAW_RULE;
  op.is_error = is_err;
  op.u.rule.x_em = x;
  op.u.rule.y_em = y;
  op.u.rule.w_em = w;
  op.u.rule.h_em = h;
  g_array_append_val(b->ops, op);
}

static double style_scale(const BtMathFont *font, BtMathStyle style)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  switch (style) {
    case BT_STYLE_DISPLAY:
    case BT_STYLE_TEXT:
      return 1.0;
    case BT_STYLE_SCRIPT:
      return c ? c->script_scale : 0.70;
    case BT_STYLE_SCRIPTSCRIPT:
    default:
      return c ? c->script_script_scale : 0.55;
  }
}

static BtMathStyle next_script_style(BtMathStyle s)
{
  if (s == BT_STYLE_DISPLAY || s == BT_STYLE_TEXT)
    return BT_STYLE_SCRIPT;
  return BT_STYLE_SCRIPTSCRIPT;
}

static BtMathStyle next_frac_style(BtMathStyle s, int override)
{
  if (override == 1) /* \dfrac */
    return BT_STYLE_DISPLAY;
  if (override == 2) /* \tfrac */
    return BT_STYLE_TEXT;
  switch (s) {
    case BT_STYLE_DISPLAY:      return BT_STYLE_TEXT;
    case BT_STYLE_TEXT:         return BT_STYLE_SCRIPT;
    case BT_STYLE_SCRIPT:
    case BT_STYLE_SCRIPTSCRIPT:
    default:                    return BT_STYLE_SCRIPTSCRIPT;
  }
}

/* TeX 8x8 inter-atom spacing table in mu (18 mu = 1 em).
 * Negative values (-3, -4, -5) mean: apply space only in D and T styles (0 in S and SS). */
static const int k_tex_space_mu[8][8] = {
  /* Right:   ORD  OP  BIN  REL OPEN CLOSE PUNCT INNER */
  /* ORD   */ {  0,  3,  -4,  -5,   0,    0,    0,   -3 },
  /* OP    */ {  3,  3,   0,  -5,   0,    0,    0,   -3 },
  /* BIN   */ { -4, -4,   0,   0,  -4,    0,    0,   -4 },
  /* REL   */ { -5, -5,   0,   0,  -5,    0,    0,   -5 },
  /* OPEN  */ {  0,  0,   0,   0,   0,    0,    0,    0 },
  /* CLOSE */ {  0,  3,  -4,  -5,   0,    0,    0,   -3 },
  /* PUNCT */ { -3, -3,   0,  -3,  -3,   -3,   -3,   -3 },
  /* INNER */ { -3,  3,  -4,  -5,  -3,    0,   -3,   -3 },
};

static double inter_atom_space_em(BtAtomClass left, BtAtomClass right, BtMathStyle style)
{
  if ((int)left < 0 || (int)left > 7 || (int)right < 0 || (int)right > 7)
    return 0.0;
  int mu = k_tex_space_mu[left][right];
  if (mu < 0) {
    if (style == BT_STYLE_SCRIPT || style == BT_STYLE_SCRIPTSCRIPT)
      return 0.0;
    mu = -mu;
  }
  return (double)mu / 18.0;
}

/* Forward declaration */
static void layout_node(const BtMathFont *font, const BtMathNode *node,
                        BtMathStyle style, gboolean cramped, MathBox *out);

static void layout_single_codepoint(const BtMathFont *font, gunichar cp,
                                    BtAtomClass cls, double scale,
                                    double min_height_em, gboolean center_on_axis,
                                    gboolean is_err, MathBox *out)
{
  box_init(out, cls);
  hb_codepoint_t gid = bt_math_font_get_glyph(font, cp);
  if (min_height_em > 0.0 && gid != 0)
    gid = bt_math_font_get_v_variant(font, gid, min_height_em, scale);

  BtGlyphMetrics gm;
  bt_math_font_get_glyph_metrics(font, gid, &gm);

  double asc = gm.ascent_em * scale;
  double desc = gm.descent_em * scale;
  double adv = gm.advance_em * scale;
  double y_shift = 0.0;

  if (center_on_axis) {
    const BtMathConstants *c = bt_math_font_get_constants(font);
    double axis = (c ? c->axis_height : 0.25) * scale;
    /* In Y-up math coords, glyph midpoint is (asc - desc)/2; we want midpoint at +axis.
     * So we shift upward by (axis - (asc - desc)/2), which in Y-down is dy = -shift_up. */
    double shift_up = axis - 0.5 * (asc - desc);
    y_shift = -shift_up;
    asc += shift_up;
    desc -= shift_up;
    if (asc < 0.0) asc = 0.0;
    if (desc < 0.0) desc = 0.0;
  }

  box_add_glyph(out, gid, cp, 0.0, y_shift, scale, is_err);
  out->width = MAX(adv, 0.0);
  out->ascent = asc;
  out->descent = desc;
  out->italic_corr = gm.italic_corr_em * scale;
  out->top_accent_x = gm.top_accent_x_em * scale;
  out->single_glyph = gid;
}

static void layout_text_run(const BtMathFont *font, const char *str,
                            BtMathFontStyle fstyle, BtAtomClass cls,
                            double scale, gboolean is_err, MathBox *out)
{
  box_init(out, cls);
  if (str == NULL || str[0] == '\0')
    return;

  double x = 0.0;
  for (const char *p = str; *p != '\0'; p = g_utf8_next_char(p)) {
    gunichar uc = g_utf8_get_char(p);
    if (uc == ' ') {
      x += 0.30 * scale;
      continue;
    }
    gunichar mapped = bt_math_map_styled_char(uc, fstyle,
                                              fstyle != BT_MATH_FONT_TEXT && fstyle != BT_MATH_FONT_ROMAN);
    hb_codepoint_t gid = bt_math_font_get_glyph(font, mapped);
    BtGlyphMetrics gm;
    bt_math_font_get_glyph_metrics(font, gid, &gm);
    box_add_glyph(out, gid, mapped, x, 0.0, scale, is_err);
    x += gm.advance_em * scale;
    out->ascent = MAX(out->ascent, gm.ascent_em * scale);
    out->descent = MAX(out->descent, gm.descent_em * scale);
  }
  out->width = x;
  out->top_accent_x = x * 0.5;
}

static void layout_large_op_glyph(const BtMathFont *font, gunichar cp,
                                  BtMathStyle style, MathBox *out)
{
  double scale = style_scale(font, style);
  /* In display style, pick a taller display-operator variant (~1.55 em) */
  double target_h = (style == BT_STYLE_DISPLAY) ? 1.55 : 0.0;
  layout_single_codepoint(font, cp, BT_ATOM_OP, scale, target_h, TRUE, FALSE, out);
  /* Include italic correction in advance width when no scripts or for limits centering */
}

static gboolean should_use_limits(const BtMathNode *base, BtMathStyle style)
{
  if (base == NULL)
    return FALSE;
  if (base->kind == BT_NODE_LARGE_OP) {
    if (base->u.large_op.limits_mode == BT_LIMITS_ALWAYS)
      return TRUE;
    if (base->u.large_op.limits_mode == BT_LIMITS_NEVER)
      return FALSE;
    return (style == BT_STYLE_DISPLAY) && base->u.large_op.movable_limits;
  }
  if (base->kind == BT_NODE_TEXT && base->u.text.is_operator) {
    if (base->u.text.limits_mode == BT_LIMITS_ALWAYS)
      return TRUE;
    if (base->u.text.limits_mode == BT_LIMITS_NEVER)
      return FALSE;
    return (style == BT_STYLE_DISPLAY) && base->u.text.movable_limits;
  }
  if (base->kind == BT_NODE_ACCENT &&
      (base->u.accent.accent_kind == BT_ACCENT_OVERBRACE ||
       base->u.accent.accent_kind == BT_ACCENT_UNDERBRACE)) {
    return TRUE;
  }
  return FALSE;
}

static void layout_scripts_node(const BtMathFont *font, const BtMathNode *node,
                                BtMathStyle style, gboolean cramped, MathBox *out)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  double scale = style_scale(font, style);
  BtMathStyle s_style = next_script_style(style);

  MathBox base_box;
  layout_node(font, node->u.scripts.base, style, cramped, &base_box);

  MathBox sub_box, sup_box;
  gboolean has_sub = (node->u.scripts.sub != NULL);
  gboolean has_sup = (node->u.scripts.sup != NULL);
  if (has_sub)
    layout_node(font, node->u.scripts.sub, s_style, TRUE, &sub_box);
  else
    box_init(&sub_box, BT_ATOM_ORD);

  if (has_sup)
    layout_node(font, node->u.scripts.sup, s_style, cramped, &sup_box);
  else
    box_init(&sup_box, BT_ATOM_ORD);

  box_init(out, base_box.atom_class);

  /* Case 1: Display limits above and below operator (\sum_{i=1}^n in display style) */
  if (should_use_limits(node->u.scripts.base, style)) {
    double max_w = base_box.width;
    if (has_sup) max_w = MAX(max_w, sup_box.width);
    if (has_sub) max_w = MAX(max_w, sub_box.width);

    double base_x = 0.5 * (max_w - base_box.width);
    box_merge_at(out, &base_box, base_x, 0.0);

    if (has_sup) {
      double gap_min = (c ? c->upper_limit_gap_min : 0.10) * scale;
      double rise_min = (c ? c->upper_limit_baseline_rise_min : 0.20) * scale;
      double shift_up = MAX(rise_min, base_box.ascent + gap_min + sup_box.descent);
      double sup_x = 0.5 * (max_w - sup_box.width) + 0.5 * base_box.italic_corr;
      box_merge_at(out, &sup_box, MAX(sup_x, 0.0), -shift_up);
    }
    if (has_sub) {
      double gap_min = (c ? c->lower_limit_gap_min : 0.10) * scale;
      double drop_min = (c ? c->lower_limit_baseline_drop_min : 0.35) * scale;
      double shift_down = MAX(drop_min, base_box.descent + gap_min + sub_box.ascent);
      double sub_x = 0.5 * (max_w - sub_box.width) - 0.5 * base_box.italic_corr;
      box_merge_at(out, &sub_box, MAX(sub_x, 0.0), +shift_down);
    }
    out->width = max_w + (c ? c->space_after_script : 0.04) * scale;
    box_clear(&base_box);
    box_clear(&sub_box);
    box_clear(&sup_box);
    return;
  }

  /* Case 2: Side superscript / subscript */
  box_merge_at(out, &base_box, 0.0, 0.0);

  double shift_up = 0.0;
  double shift_down = 0.0;

  if (has_sup) {
    double std_up = (cramped ? (c ? c->sup_shift_up_cramped : 0.29)
                             : (c ? c->sup_shift_up : 0.36)) * scale;
    double min_up_from_base = (base_box.ascent > 0.55 * scale)
                                  ? (base_box.ascent - 0.25 * scale)
                                  : 0.0;
    double bot_min = (c ? c->sup_bottom_min : 0.12) * scale;
    shift_up = MAX(std_up, min_up_from_base);
    shift_up = MAX(shift_up, bot_min + sup_box.descent);
  }

  if (has_sub) {
    double std_down = (c ? c->sub_shift_down : 0.20) * scale;
    double min_down_from_base = (base_box.descent > 0.15 * scale)
                                    ? (base_box.descent + 0.05 * scale)
                                    : 0.0;
    double top_max = (c ? c->sub_top_max : 0.36) * scale;
    shift_down = MAX(std_down, min_down_from_base);
    shift_down = MAX(shift_down, sub_box.ascent - top_max);
  }

  /* Enforce SubSuperscriptGapMin when both sub and sup are present */
  if (has_sup && has_sub) {
    double gap_min = (c ? c->sub_sup_gap_min : 0.15) * scale;
    /* Gap between bottom of sup (at height shift_up - sup_box.descent)
     * and top of sub (at height -shift_down + sub_box.ascent): */
    double gap = (shift_up - sup_box.descent) - (sub_box.ascent - shift_down);
    if (gap < gap_min) {
      double needed = gap_min - gap;
      double max_bot = (c ? c->sup_bottom_max_with_sub : 0.36) * scale;
      double can_raise = MAX(0.0, max_bot - (shift_up - sup_box.descent));
      double raise = MIN(needed, can_raise);
      shift_up += raise;
      shift_down += (needed - raise);
    }
  }

  double script_x = base_box.width;
  double sup_x = script_x + (base_box.italic_corr > 0.0 ? base_box.italic_corr * 0.45 : 0.0);
  double sub_x = script_x;
  /* For large operators like \int, pull subscript slightly left by italic correction */
  if (node->u.scripts.base != NULL && node->u.scripts.base->kind == BT_NODE_LARGE_OP &&
      base_box.italic_corr > 0.0) {
    sub_x = MAX(0.0, script_x - base_box.italic_corr * 0.40);
  }

  if (has_sup)
    box_merge_at(out, &sup_box, sup_x, -shift_up);
  if (has_sub)
    box_merge_at(out, &sub_box, sub_x, +shift_down);

  out->width += (c ? c->space_after_script : 0.04) * scale;

  box_clear(&base_box);
  box_clear(&sub_box);
  box_clear(&sup_box);
}

static void layout_frac_node(const BtMathFont *font, const BtMathNode *node,
                             BtMathStyle style, gboolean cramped, MathBox *out)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  BtMathStyle eff_style = style;
  if (node->u.frac.style_override == 1)
    eff_style = BT_STYLE_DISPLAY;
  else if (node->u.frac.style_override == 2)
    eff_style = BT_STYLE_TEXT;

  double scale = style_scale(font, eff_style);
  gboolean is_disp = (eff_style == BT_STYLE_DISPLAY);
  BtMathStyle child_style = next_frac_style(style, node->u.frac.style_override);

  MathBox num_box, den_box;
  layout_node(font, node->u.frac.num, child_style, cramped, &num_box);
  layout_node(font, node->u.frac.den, child_style, TRUE, &den_box);

  double axis = (c ? c->axis_height : 0.25) * scale;
  double rule_h = node->u.frac.has_bar ? ((c ? c->frac_rule_thickness : 0.045) * scale) : 0.0;

  double shift_up, shift_down;
  if (node->u.frac.has_bar) {
    shift_up = (is_disp ? (c ? c->frac_num_disp_shift_up : 0.68)
                        : (c ? c->frac_num_shift_up : 0.42)) * scale;
    shift_down = (is_disp ? (c ? c->frac_den_disp_shift_down : 0.68)
                          : (c ? c->frac_den_shift_down : 0.42)) * scale;
    double num_gap_min = (is_disp ? (c ? c->frac_num_disp_gap_min : 0.14)
                                  : (c ? c->frac_num_gap_min : 0.06)) * scale;
    double den_gap_min = (is_disp ? (c ? c->frac_den_disp_gap_min : 0.14)
                                  : (c ? c->frac_den_gap_min : 0.06)) * scale;

    /* Enforce gap between numerator bottom and top of fraction rule (axis + rule_h/2) */
    double min_up = axis + 0.5 * rule_h + num_gap_min + num_box.descent;
    if (shift_up < min_up)
      shift_up = min_up;

    /* Enforce gap between bottom of fraction rule (axis - rule_h/2) and denominator top */
    double min_down = -axis + 0.5 * rule_h + den_gap_min + den_box.ascent;
    if (shift_down < min_down)
      shift_down = min_down;
  } else {
    /* Binomial / stack without rule */
    shift_up = (is_disp ? (c ? c->stack_top_disp_shift_up : 0.70)
                        : (c ? c->stack_top_shift_up : 0.45)) * scale;
    shift_down = (is_disp ? (c ? c->stack_bot_disp_shift_down : 0.65)
                          : (c ? c->stack_bot_shift_down : 0.38)) * scale;
    double gap_min = (is_disp ? (c ? c->stack_disp_gap_min : 0.30)
                              : (c ? c->stack_gap_min : 0.15)) * scale;
    double gap = (shift_up - num_box.descent) - (den_box.ascent - shift_down);
    if (gap < gap_min) {
      double add = 0.5 * (gap_min - gap);
      shift_up += add;
      shift_down += add;
    }
  }

  double pad = 0.08 * scale;
  double content_w = MAX(num_box.width, den_box.width) + 2.0 * pad;

  MathBox inner;
  box_init(&inner, BT_ATOM_INNER);
  box_merge_at(&inner, &num_box, 0.5 * (content_w - num_box.width), -shift_up);
  box_merge_at(&inner, &den_box, 0.5 * (content_w - den_box.width), +shift_down);
  if (node->u.frac.has_bar)
    box_add_rule(&inner, 0.0, -(axis + 0.5 * rule_h), content_w, rule_h, FALSE);
  inner.width = content_w;

  /* Optional surrounding delimiters (e.g. \binom{n}{k}) */
  if (node->u.frac.left_delim != 0 || node->u.frac.right_delim != 0) {
    box_init(out, BT_ATOM_INNER);
    double target_h = inner.ascent + inner.descent;
    double cur_x = 0.0;
    if (node->u.frac.left_delim != 0) {
      MathBox lb;
      layout_single_codepoint(font, node->u.frac.left_delim, BT_ATOM_OPEN,
                              scale, target_h, TRUE, FALSE, &lb);
      box_merge_at(out, &lb, cur_x, 0.0);
      cur_x += lb.width;
      box_clear(&lb);
    }
    box_merge_at(out, &inner, cur_x, 0.0);
    cur_x += inner.width;
    if (node->u.frac.right_delim != 0) {
      MathBox rb;
      layout_single_codepoint(font, node->u.frac.right_delim, BT_ATOM_CLOSE,
                              scale, target_h, TRUE, FALSE, &rb);
      box_merge_at(out, &rb, cur_x, 0.0);
      cur_x += rb.width;
      box_clear(&rb);
    }
    out->width = cur_x;
    box_clear(&inner);
  } else {
    *out = inner;
  }

  box_clear(&num_box);
  box_clear(&den_box);
}

static void layout_sqrt_node(const BtMathFont *font, const BtMathNode *node,
                             BtMathStyle style, MathBox *out)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  double scale = style_scale(font, style);

  MathBox rad_box;
  layout_node(font, node->u.sqrt_node.radicand, style, TRUE, &rad_box);

  double rule_h = (c ? c->radical_rule_thickness : 0.045) * scale;
  double vgap = ((style == BT_STYLE_DISPLAY) ? (c ? c->radical_disp_vgap : 0.12)
                                             : (c ? c->radical_vgap : 0.06)) * scale;
  double extra_asc = (c ? c->radical_extra_ascender : 0.05) * scale;
  double target_h = rad_box.ascent + rad_box.descent + vgap + rule_h;

  hb_codepoint_t surd_gid = bt_math_font_get_glyph(font, 0x221Au);
  surd_gid = bt_math_font_get_v_variant(font, surd_gid, target_h, scale);
  BtGlyphMetrics gm;
  bt_math_font_get_glyph_metrics(font, surd_gid, &gm);

  double surd_asc = gm.ascent_em * scale;
  double surd_desc = gm.descent_em * scale;
  double surd_w = gm.advance_em * scale;

  /* Align top of surd glyph with the top of the radical rule:
   * Top of rule in Y-up is rule_top = rad_box.ascent + vgap + rule_h.
   * If surd is taller than target_h, center the extra below/above smoothly. */
  double surd_total = surd_asc + surd_desc;
  if (surd_total > target_h)
    vgap += 0.5 * (surd_total - target_h);

  double rule_top_up = rad_box.ascent + vgap + rule_h;
  /* Shift surd so its top (surd_asc + shift_up) equals rule_top_up: */
  double surd_shift_up = rule_top_up - surd_asc;

  box_init(out, BT_ATOM_ORD);
  double cur_x = 0.0;

  /* Optional root index \sqrt[n]{x} */
  if (node->u.sqrt_node.index != NULL) {
    MathBox idx_box;
    layout_node(font, node->u.sqrt_node.index, BT_STYLE_SCRIPTSCRIPT, TRUE, &idx_box);
    double k_before = MAX((c ? c->radical_kern_before_deg : 0.10) * scale, 0.05 * scale);
    double k_after = (c ? c->radical_kern_after_deg : -0.35) * scale;
    double raise = (c ? c->radical_deg_bottom_raise_pct : 0.60) * (rule_top_up + rad_box.descent) - rad_box.descent;
    cur_x += k_before;
    box_merge_at(out, &idx_box, cur_x, -(raise + idx_box.descent));
    cur_x = MAX(0.0, cur_x + idx_box.width + k_after);
    box_clear(&idx_box);
  }

  /* Surd glyph */
  box_add_glyph(out, surd_gid, 0x221Au, cur_x, -surd_shift_up, scale, FALSE);
  out->ascent = MAX(out->ascent, surd_asc + surd_shift_up + extra_asc);
  out->descent = MAX(out->descent, surd_desc - surd_shift_up);
  cur_x += surd_w;

  /* Vinculum rule + radicand */
  double bar_w = rad_box.width + 0.06 * scale;
  box_add_rule(out, cur_x, -rule_top_up, bar_w, rule_h, FALSE);
  box_merge_at(out, &rad_box, cur_x + 0.03 * scale, 0.0);
  out->ascent = MAX(out->ascent, rule_top_up + extra_asc);
  out->width = cur_x + bar_w;

  box_clear(&rad_box);
}

static void layout_accent_node(const BtMathFont *font, const BtMathNode *node,
                               BtMathStyle style, gboolean cramped, MathBox *out)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  double scale = style_scale(font, style);

  MathBox base_box;
  layout_node(font, node->u.accent.base, style, TRUE, &base_box);
  (void)cramped;

  BtAccentKind ak = node->u.accent.accent_kind;
  box_init(out, node->atom_class);

  /* Overline: horizontal rule above base */
  if (ak == BT_ACCENT_OVERLINE) {
    double vgap = (c ? c->overbar_vgap : 0.12) * scale;
    double rule_h = (c ? c->overbar_rule_thickness : 0.045) * scale;
    double extra = (c ? c->overbar_extra_ascender : 0.04) * scale;
    box_merge_at(out, &base_box, 0.0, 0.0);
    double rule_top = base_box.ascent + vgap + rule_h;
    box_add_rule(out, 0.0, -rule_top, MAX(base_box.width, 0.25 * scale), rule_h, FALSE);
    out->ascent = MAX(out->ascent, rule_top + extra);
    box_clear(&base_box);
    return;
  }

  /* Underline: horizontal rule below base */
  if (ak == BT_ACCENT_UNDERLINE) {
    double vgap = (c ? c->underbar_vgap : 0.12) * scale;
    double rule_h = (c ? c->underbar_rule_thickness : 0.045) * scale;
    double extra = (c ? c->underbar_extra_descender : 0.04) * scale;
    box_merge_at(out, &base_box, 0.0, 0.0);
    double rule_top_down = base_box.descent + vgap;
    box_add_rule(out, 0.0, +rule_top_down, MAX(base_box.width, 0.25 * scale), rule_h, FALSE);
    out->descent = MAX(out->descent, rule_top_down + rule_h + extra);
    box_clear(&base_box);
    return;
  }

  /* Underbrace */
  if (ak == BT_ACCENT_UNDERBRACE) {
    hb_codepoint_t gid = bt_math_font_get_glyph(font, 0x23DFu);
    gid = bt_math_font_get_h_variant(font, gid, base_box.width, scale);
    BtGlyphMetrics gm;
    bt_math_font_get_glyph_metrics(font, gid, &gm);
    double w = MAX(base_box.width, gm.advance_em * scale);
    box_merge_at(out, &base_box, 0.5 * (w - base_box.width), 0.0);
    double shift_down = base_box.descent + 0.08 * scale + gm.ascent_em * scale;
    box_add_glyph(out, gid, 0x23DFu, 0.5 * (w - gm.advance_em * scale), +shift_down, scale, FALSE);
    out->descent = MAX(out->descent, shift_down + gm.descent_em * scale);
    out->width = w;
    box_clear(&base_box);
    return;
  }

  /* Overbrace */
  if (ak == BT_ACCENT_OVERBRACE) {
    hb_codepoint_t gid = bt_math_font_get_glyph(font, 0x23DEu);
    gid = bt_math_font_get_h_variant(font, gid, base_box.width, scale);
    BtGlyphMetrics gm;
    bt_math_font_get_glyph_metrics(font, gid, &gm);
    double w = MAX(base_box.width, gm.advance_em * scale);
    box_merge_at(out, &base_box, 0.5 * (w - base_box.width), 0.0);
    double shift_up = base_box.ascent + 0.08 * scale + gm.descent_em * scale;
    box_add_glyph(out, gid, 0x23DEu, 0.5 * (w - gm.advance_em * scale), -shift_up, scale, FALSE);
    out->ascent = MAX(out->ascent, shift_up + gm.ascent_em * scale);
    out->width = w;
    box_clear(&base_box);
    return;
  }

  /* Standard top accents: \bar, \hat, \tilde, \vec, \dot, \ddot, \widehat, \widetilde */
  box_merge_at(out, &base_box, 0.0, 0.0);

  hb_codepoint_t acc_gid = bt_math_font_get_glyph(font, node->u.accent.accent_cp);
  if (ak == BT_ACCENT_WIDEHAT || ak == BT_ACCENT_WIDETILDE || base_box.width > 0.75 * scale)
    acc_gid = bt_math_font_get_h_variant(font, acc_gid, base_box.width, scale);

  BtGlyphMetrics gm;
  bt_math_font_get_glyph_metrics(font, acc_gid, &gm);

  /* Combining accents in STIXTwoMath are pre-positioned around accent_base_height (~x-height).
   * Lift the accent by max(0, base_box.ascent - accent_base_height). */
  double base_h = (c ? c->accent_base_height : 0.48) * scale;
  double shift_up = MAX(0.0, base_box.ascent - base_h);

  /* If the accent glyph itself has low y_bearing (non-combining fallback), lift it above base */
  if (gm.ascent_em * scale < base_h * 0.9)
    shift_up = base_box.ascent + 0.06 * scale;

  double base_attach = (base_box.top_accent_x > 0.0) ? base_box.top_accent_x : (0.5 * base_box.width);
  double acc_attach = (gm.top_accent_x_em != 0.0) ? (gm.top_accent_x_em * scale)
                                                  : (0.5 * (gm.left_bearing_em + gm.right_bearing_em) * scale);
  double acc_x = base_attach - acc_attach;

  box_add_glyph(out, acc_gid, node->u.accent.accent_cp, acc_x, -shift_up, scale, FALSE);
  out->ascent = MAX(out->ascent, gm.ascent_em * scale + shift_up + 0.02 * scale);
  out->italic_corr = base_box.italic_corr;
  out->top_accent_x = base_attach;

  box_clear(&base_box);
}

static void layout_delim_group_node(const BtMathFont *font, const BtMathNode *node,
                                    BtMathStyle style, gboolean cramped, MathBox *out)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  double scale = style_scale(font, style);
  double axis = (c ? c->axis_height : 0.25) * scale;

  MathBox body_box;
  layout_node(font, node->u.delim_group.body, style, cramped, &body_box);

  /* Symmetric height around math axis per TeX rule */
  double max_extent = MAX(body_box.ascent - axis, body_box.descent + axis);
  double target_h = MAX(2.0 * max_extent, 0.85 * scale);

  box_init(out, BT_ATOM_INNER);
  double cur_x = 0.0;

  if (node->u.delim_group.left_delim != 0) {
    MathBox lb;
    layout_single_codepoint(font, node->u.delim_group.left_delim, BT_ATOM_OPEN,
                            scale, target_h, TRUE, FALSE, &lb);
    box_merge_at(out, &lb, cur_x, 0.0);
    cur_x += lb.width;
    box_clear(&lb);
  }

  box_merge_at(out, &body_box, cur_x, 0.0);
  cur_x += body_box.width;

  if (node->u.delim_group.right_delim != 0) {
    MathBox rb;
    layout_single_codepoint(font, node->u.delim_group.right_delim, BT_ATOM_CLOSE,
                            scale, target_h, TRUE, FALSE, &rb);
    box_merge_at(out, &rb, cur_x, 0.0);
    cur_x += rb.width;
    box_clear(&rb);
  }

  out->width = cur_x;
  box_clear(&body_box);
}

static void layout_matrix_node(const BtMathFont *font, const BtMathNode *node,
                               BtMathStyle style, gboolean cramped, MathBox *out)
{
  const BtMathConstants *c = bt_math_font_get_constants(font);
  double scale = style_scale(font, style);
  double axis = (c ? c->axis_height : 0.25) * scale;
  BtMathStyle cell_style = (node->u.matrix.is_aligned || node->u.matrix.is_cases)
                               ? style
                               : ((style == BT_STYLE_DISPLAY) ? BT_STYLE_TEXT : style);

  guint rows = node->u.matrix.rows;
  guint cols = node->u.matrix.cols;
  if (rows == 0 || cols == 0) {
    box_init(out, BT_ATOM_INNER);
    return;
  }

  MathBox *cells = g_new0(MathBox, rows * cols);
  double *col_w = g_new0(double, cols);
  double *row_asc = g_new0(double, rows);
  double *row_desc = g_new0(double, rows);

  for (guint r = 0; r < rows; r++) {
    row_asc[r] = 0.55 * scale;
    row_desc[r] = 0.20 * scale;
    for (guint col = 0; col < cols; col++) {
      MathBox *cb = &cells[r * cols + col];
      layout_node(font, node->u.matrix.cells[r][col], cell_style, cramped, cb);
      col_w[col] = MAX(col_w[col], cb->width);
      row_asc[r] = MAX(row_asc[r], cb->ascent);
      row_desc[r] = MAX(row_desc[r], cb->descent);
    }
  }

  double col_gap = (node->u.matrix.is_cases ? 0.70 : 0.65) * scale;
  double row_gap = 0.30 * scale;

  double total_w = 0.0;
  for (guint col = 0; col < cols; col++) {
    total_w += col_w[col];
    if (col + 1 < cols) {
      /* In aligned environments, alternating columns (r&l, r&l) have 0 gap at & and quad between pairs */
      if (node->u.matrix.is_aligned && (col % 2 == 0))
        total_w += 0.05 * scale;
      else
        total_w += col_gap;
    }
  }

  double total_h = 0.0;
  for (guint r = 0; r < rows; r++) {
    total_h += row_asc[r] + row_desc[r];
    if (r + 1 < rows)
      total_h += row_gap;
  }

  /* Center the matrix grid vertically on the math axis:
   * Top of grid in Y-down is y_top = -(axis + 0.5 * total_h). */
  double y_cursor = -(axis + 0.5 * total_h);

  MathBox grid;
  box_init(&grid, BT_ATOM_INNER);

  for (guint r = 0; r < rows; r++) {
    double row_baseline_y = y_cursor + row_asc[r];
    double x_cursor = 0.0;
    for (guint col = 0; col < cols; col++) {
      MathBox *cb = &cells[r * cols + col];
      double cell_x = x_cursor;
      if (node->u.matrix.is_cases) {
        /* Left-align both columns in cases */
        cell_x = x_cursor;
      } else if (node->u.matrix.is_aligned) {
        /* Right-align even columns (0, 2..), left-align odd columns (1, 3..) */
        if (col % 2 == 0)
          cell_x = x_cursor + (col_w[col] - cb->width);
        else
          cell_x = x_cursor;
      } else {
        /* Center matrix columns */
        cell_x = x_cursor + 0.5 * (col_w[col] - cb->width);
      }
      box_merge_at(&grid, cb, cell_x, row_baseline_y);
      x_cursor += col_w[col];
      if (col + 1 < cols) {
        if (node->u.matrix.is_aligned && (col % 2 == 0))
          x_cursor += 0.05 * scale;
        else
          x_cursor += col_gap;
      }
      box_clear(cb);
    }
    y_cursor += row_asc[r] + row_desc[r] + row_gap;
  }
  grid.width = total_w;

  g_free(cells);
  g_free(col_w);
  g_free(row_asc);
  g_free(row_desc);

  /* Wrap with matrix delimiters if any */
  if (node->u.matrix.left_delim != 0 || node->u.matrix.right_delim != 0) {
    box_init(out, BT_ATOM_INNER);
    double target_h = grid.ascent + grid.descent;
    double cur_x = 0.0;
    if (node->u.matrix.left_delim != 0) {
      MathBox lb;
      layout_single_codepoint(font, node->u.matrix.left_delim, BT_ATOM_OPEN,
                              scale, target_h, TRUE, FALSE, &lb);
      box_merge_at(out, &lb, cur_x, 0.0);
      cur_x += lb.width + 0.08 * scale;
      box_clear(&lb);
    }
    box_merge_at(out, &grid, cur_x, 0.0);
    cur_x += grid.width;
    if (node->u.matrix.right_delim != 0) {
      cur_x += 0.08 * scale;
      MathBox rb;
      layout_single_codepoint(font, node->u.matrix.right_delim, BT_ATOM_CLOSE,
                              scale, target_h, TRUE, FALSE, &rb);
      box_merge_at(out, &rb, cur_x, 0.0);
      cur_x += rb.width;
      box_clear(&rb);
    }
    out->width = cur_x;
    box_clear(&grid);
  } else {
    *out = grid;
  }
}

static void layout_overunder_node(const BtMathFont *font, const BtMathNode *node,
                                  BtMathStyle style, gboolean cramped, MathBox *out)
{
  double scale = style_scale(font, style);
  BtMathStyle s_style = next_script_style(style);

  MathBox base_box, over_box, under_box;
  layout_node(font, node->u.overunder.base, style, cramped, &base_box);
  gboolean has_over = (node->u.overunder.over != NULL);
  gboolean has_under = (node->u.overunder.under != NULL);

  if (has_over)
    layout_node(font, node->u.overunder.over, s_style, cramped, &over_box);
  else
    box_init(&over_box, BT_ATOM_ORD);

  if (has_under)
    layout_node(font, node->u.overunder.under, s_style, TRUE, &under_box);
  else
    box_init(&under_box, BT_ATOM_ORD);

  double max_w = base_box.width;
  if (has_over) max_w = MAX(max_w, over_box.width);
  if (has_under) max_w = MAX(max_w, under_box.width);

  box_init(out, node->atom_class);
  box_merge_at(out, &base_box, 0.5 * (max_w - base_box.width), 0.0);

  if (has_over) {
    double shift_up = base_box.ascent + 0.10 * scale + over_box.descent;
    box_merge_at(out, &over_box, 0.5 * (max_w - over_box.width), -shift_up);
  }
  if (has_under) {
    double shift_down = base_box.descent + 0.10 * scale + under_box.ascent;
    box_merge_at(out, &under_box, 0.5 * (max_w - under_box.width), +shift_down);
  }
  out->width = max_w;

  box_clear(&base_box);
  box_clear(&over_box);
  box_clear(&under_box);
}

static void layout_row_node(const BtMathFont *font, const BtMathNode *node,
                            BtMathStyle style, gboolean cramped, MathBox *out)
{
  box_init(out, BT_ATOM_ORD);
  guint n = node->u.row.count;
  if (n == 0)
    return;

  if (n == 1) {
    box_clear(out);
    layout_node(font, node->u.row.items[0], style, cramped, out);
    return;
  }

  BtMathStyle cur_style = style;
  MathBox *boxes = g_new0(MathBox, n);
  gboolean *is_space = g_new0(gboolean, n);
  gboolean *is_switch = g_new0(gboolean, n);

  for (guint i = 0; i < n; i++) {
    const BtMathNode *child = node->u.row.items[i];
    if (child != NULL && child->kind == BT_NODE_STYLE_SWITCH) {
      is_switch[i] = TRUE;
      cur_style = (BtMathStyle)CLAMP(child->u.style_switch.math_style, 0, 3);
      box_init(&boxes[i], BT_ATOM_ORD);
      continue;
    }
    if (child != NULL && child->kind == BT_NODE_SPACE)
      is_space[i] = TRUE;
    layout_node(font, child, cur_style, cramped, &boxes[i]);
  }

  /* TeX Rule 5 & 14: resolve BIN -> ORD in unary context:
   * A BIN atom becomes ORD if:
   * - it is preceded by no atom or by BIN, OP, REL, OPEN, PUNCT
   * - OR it is followed by no atom or by REL, CLOSE, PUNCT */
  BtAtomClass prev_cls = BT_ATOM_OPEN; /* start of list acts like OPEN */
  for (guint i = 0; i < n; i++) {
    if (is_switch[i] || is_space[i])
      continue;
    if (boxes[i].atom_class == BT_ATOM_BIN) {
      if (prev_cls == BT_ATOM_BIN || prev_cls == BT_ATOM_OP ||
          prev_cls == BT_ATOM_REL || prev_cls == BT_ATOM_OPEN ||
          prev_cls == BT_ATOM_PUNCT) {
        boxes[i].atom_class = BT_ATOM_ORD;
      }
    }
    prev_cls = boxes[i].atom_class;
  }

  BtAtomClass next_cls = BT_ATOM_CLOSE; /* end of list acts like CLOSE */
  for (int i = (int)n - 1; i >= 0; i--) {
    if (is_switch[i] || is_space[i])
      continue;
    if (boxes[i].atom_class == BT_ATOM_BIN) {
      if (next_cls == BT_ATOM_REL || next_cls == BT_ATOM_CLOSE ||
          next_cls == BT_ATOM_PUNCT) {
        boxes[i].atom_class = BT_ATOM_ORD;
      }
    }
    next_cls = boxes[i].atom_class;
  }

  double x = 0.0;
  gboolean have_prev = FALSE;
  BtAtomClass last_cls = BT_ATOM_ORD;
  cur_style = style;

  for (guint i = 0; i < n; i++) {
    if (is_switch[i]) {
      const BtMathNode *child = node->u.row.items[i];
      cur_style = (BtMathStyle)CLAMP(child->u.style_switch.math_style, 0, 3);
      box_clear(&boxes[i]);
      continue;
    }
    if (is_space[i]) {
      x += boxes[i].width;
      box_clear(&boxes[i]);
      continue;
    }
    if (have_prev) {
      double sp = inter_atom_space_em(last_cls, boxes[i].atom_class, cur_style);
      x += sp * style_scale(font, cur_style);
    }
    box_merge_at(out, &boxes[i], x, 0.0);
    x += boxes[i].width;
    out->italic_corr = boxes[i].italic_corr;
    last_cls = boxes[i].atom_class;
    have_prev = TRUE;
    box_clear(&boxes[i]);
  }
  out->width = MAX(x, 0.0);
  out->top_accent_x = 0.5 * out->width;

  g_free(boxes);
  g_free(is_space);
  g_free(is_switch);
}

static void layout_node(const BtMathFont *font, const BtMathNode *node,
                        BtMathStyle style, gboolean cramped, MathBox *out)
{
  if (node == NULL) {
    box_init(out, BT_ATOM_ORD);
    return;
  }

  double scale = style_scale(font, style);

  switch (node->kind) {
    case BT_NODE_ROW:
      layout_row_node(font, node, style, cramped, out);
      break;

    case BT_NODE_SYMBOL: {
      double min_h = 0.0;
      gboolean center_axis = FALSE;
      if (node->u.symbol.big_step > 0) {
        /* \big=1.2em, \Big=1.6em, \bigg=2.1em, \Bigg=2.6em */
        static const double k_big_h[] = { 0.0, 1.20, 1.60, 2.10, 2.60 };
        int idx = CLAMP(node->u.symbol.big_step, 1, 4);
        min_h = k_big_h[idx] * scale;
        center_axis = TRUE;
      }
      layout_single_codepoint(font, node->u.symbol.codepoint, node->atom_class,
                              scale, min_h, center_axis, FALSE, out);
      break;
    }

    case BT_NODE_TEXT:
      layout_text_run(font, node->u.text.text, node->u.text.font_style,
                      node->atom_class, scale, FALSE, out);
      break;

    case BT_NODE_LARGE_OP:
      layout_large_op_glyph(font, node->u.large_op.codepoint, style, out);
      break;

    case BT_NODE_SCRIPTS:
      layout_scripts_node(font, node, style, cramped, out);
      break;

    case BT_NODE_FRAC:
      layout_frac_node(font, node, style, cramped, out);
      break;

    case BT_NODE_SQRT:
      layout_sqrt_node(font, node, style, out);
      break;

    case BT_NODE_ACCENT:
      layout_accent_node(font, node, style, cramped, out);
      break;

    case BT_NODE_DELIM_GROUP:
      layout_delim_group_node(font, node, style, cramped, out);
      break;

    case BT_NODE_MATRIX:
      layout_matrix_node(font, node, style, cramped, out);
      break;

    case BT_NODE_SPACE:
      box_init(out, BT_ATOM_ORD);
      out->width = ((double)node->u.space.mu / 18.0) * scale;
      break;

    case BT_NODE_STYLE_SWITCH:
      box_init(out, BT_ATOM_ORD);
      break;

    case BT_NODE_OVERUNDER:
      layout_overunder_node(font, node, style, cramped, out);
      break;

    case BT_NODE_ERROR:
      layout_text_run(font, node->u.error.label ? node->u.error.label : "?",
                      BT_MATH_FONT_ROMAN, BT_ATOM_ORD, scale, TRUE, out);
      break;
  }
}

BtMathLayout *bt_math_layout_build(const BtMathFont *font, const BtMathParseResult *ast)
{
  if (font == NULL || ast == NULL)
    return NULL;

  BtMathStyle init_style = (ast->mode == BT_MATH_DISPLAY) ? BT_STYLE_DISPLAY : BT_STYLE_TEXT;
  MathBox root_box;
  layout_node(font, ast->root, init_style, FALSE, &root_box);

  BtMathLayout *lay = g_new0(BtMathLayout, 1);
  lay->width_em = MAX(root_box.width, 0.10);
  lay->ascent_em = MAX(root_box.ascent, 0.50);
  lay->descent_em = MAX(root_box.descent, 0.15);
  lay->mode = ast->mode;
  lay->ops = root_box.ops; /* transfer ownership */
  return lay;
}

BtMathLayout *bt_math_layout_from_latex(const BtMathFont *font, const char *src, gsize len,
                                        BtMathMode mode)
{
  if (font == NULL)
    font = bt_math_font_get_default();
  if (font == NULL)
    return NULL;
  BtMathParseResult *ast = bt_math_parse(src, len, mode);
  BtMathLayout *lay = bt_math_layout_build(font, ast);
  bt_math_parse_result_free(ast);
  return lay;
}

void bt_math_layout_free(BtMathLayout *layout)
{
  if (layout == NULL)
    return;
  if (layout->ops != NULL)
    g_array_unref(layout->ops);
  g_free(layout);
}
