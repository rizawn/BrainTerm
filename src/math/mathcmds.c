/* mathcmds.c — LaTeX command registry, symbol mappings, and atom classes (M2). */
#include "mathcmds.h"

#include <string.h>

/* aux flags for LARGE_OP: bit 0 = movable_limits (true for \sum, \prod; false for \int) */
#define OP_FIXED_LIMITS   0
#define OP_MOVABLE_LIMITS 1

/* aux flags for FRAC: 0 = auto, 1 = display (\dfrac), 2 = text (\tfrac) */
#define FRAC_AUTO    0
#define FRAC_DISPLAY 1
#define FRAC_TEXT    2

static const BtMathCmdEntry k_cmds[] = {
  /* ---- Security blocklist (FORBIDDEN TeX primitives, §B.17) ---- */
  { "input",            BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "include",          BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "write",            BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "read",             BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "openin",           BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "openout",          BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "closein",          BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "closeout",         BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "immediate",        BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "special",          BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "catcode",          BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "csname",           BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "endcsname",        BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "expandafter",      BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "def",              BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "edef",             BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "gdef",             BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "xdef",             BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "let",              BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "futurelet",        BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "newcommand",       BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "renewcommand",     BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "providecommand",   BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "DeclareMathOperator", BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "loop",             BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "repeat",           BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "makeatletter",     BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },
  { "makeatother",      BT_CMD_FORBIDDEN, 0, BT_ATOM_ORD, 0, NULL },

  /* ---- Structural commands ---- */
  { "frac",             BT_CMD_FRAC,  0, BT_ATOM_INNER, FRAC_AUTO, NULL },
  { "dfrac",            BT_CMD_FRAC,  0, BT_ATOM_INNER, FRAC_DISPLAY, NULL },
  { "tfrac",            BT_CMD_FRAC,  0, BT_ATOM_INNER, FRAC_TEXT, NULL },
  { "cfrac",            BT_CMD_FRAC,  0, BT_ATOM_INNER, FRAC_DISPLAY, NULL },
  { "binom",            BT_CMD_BINOM, 0, BT_ATOM_INNER, FRAC_AUTO, NULL },
  { "dbinom",           BT_CMD_BINOM, 0, BT_ATOM_INNER, FRAC_DISPLAY, NULL },
  { "tbinom",           BT_CMD_BINOM, 0, BT_ATOM_INNER, FRAC_TEXT, NULL },
  { "sqrt",             BT_CMD_SQRT,  0x221A, BT_ATOM_ORD, 0, NULL },
  { "left",             BT_CMD_LEFT,  0, BT_ATOM_OPEN, 0, NULL },
  { "right",            BT_CMD_RIGHT, 0, BT_ATOM_CLOSE, 0, NULL },
  { "middle",           BT_CMD_MIDDLE, 0, BT_ATOM_REL, 0, NULL },
  { "begin",            BT_CMD_BEGIN, 0, BT_ATOM_ORD, 0, NULL },
  { "end",              BT_CMD_END,   0, BT_ATOM_ORD, 0, NULL },
  { "overset",          BT_CMD_OVERSET, 0, BT_ATOM_REL, 0, NULL },
  { "stackrel",         BT_CMD_OVERSET, 0, BT_ATOM_REL, 0, NULL },
  { "underset",         BT_CMD_UNDERSET, 0, BT_ATOM_REL, 0, NULL },
  { "phantom",          BT_CMD_PHANTOM, 0, BT_ATOM_ORD, 0, NULL },
  { "hphantom",         BT_CMD_PHANTOM, 0, BT_ATOM_ORD, 1, NULL },
  { "vphantom",         BT_CMD_PHANTOM, 0, BT_ATOM_ORD, 2, NULL },
  { "boxed",            BT_CMD_COLOR_NOOP, 0, BT_ATOM_ORD, 1, NULL },
  { "color",            BT_CMD_COLOR_NOOP, 0, BT_ATOM_ORD, 2, NULL },
  { "textcolor",        BT_CMD_COLOR_NOOP, 0, BT_ATOM_ORD, 3, NULL },
  { "not",              BT_CMD_NOT, 0, BT_ATOM_REL, 0, NULL },
  { "bmod",             BT_CMD_MOD, 0, BT_ATOM_BIN, 0, "mod" },
  { "pmod",             BT_CMD_MOD, 0, BT_ATOM_ORD, 1, "mod" },
  { "mod",              BT_CMD_MOD, 0, BT_ATOM_BIN, 2, "mod" },

  /* ---- Big delimiters (\big, \Big, \bigg, \Bigg) ---- */
  { "big",              BT_CMD_BIG_DELIM, 0, BT_ATOM_ORD,   1, NULL },
  { "bigl",             BT_CMD_BIG_DELIM, 0, BT_ATOM_OPEN,  1, NULL },
  { "bigr",             BT_CMD_BIG_DELIM, 0, BT_ATOM_CLOSE, 1, NULL },
  { "bigm",             BT_CMD_BIG_DELIM, 0, BT_ATOM_REL,   1, NULL },
  { "Big",              BT_CMD_BIG_DELIM, 0, BT_ATOM_ORD,   2, NULL },
  { "Bigl",             BT_CMD_BIG_DELIM, 0, BT_ATOM_OPEN,  2, NULL },
  { "Bigr",             BT_CMD_BIG_DELIM, 0, BT_ATOM_CLOSE, 2, NULL },
  { "Bigm",             BT_CMD_BIG_DELIM, 0, BT_ATOM_REL,   2, NULL },
  { "bigg",             BT_CMD_BIG_DELIM, 0, BT_ATOM_ORD,   3, NULL },
  { "biggl",            BT_CMD_BIG_DELIM, 0, BT_ATOM_OPEN,  3, NULL },
  { "biggr",            BT_CMD_BIG_DELIM, 0, BT_ATOM_CLOSE, 3, NULL },
  { "biggm",            BT_CMD_BIG_DELIM, 0, BT_ATOM_REL,   3, NULL },
  { "Bigg",             BT_CMD_BIG_DELIM, 0, BT_ATOM_ORD,   4, NULL },
  { "Biggl",            BT_CMD_BIG_DELIM, 0, BT_ATOM_OPEN,  4, NULL },
  { "Biggr",            BT_CMD_BIG_DELIM, 0, BT_ATOM_CLOSE, 4, NULL },
  { "Biggm",            BT_CMD_BIG_DELIM, 0, BT_ATOM_REL,   4, NULL },

  /* ---- Style & limit switches ---- */
  { "displaystyle",         BT_CMD_STYLE_SWITCH, 0, BT_ATOM_ORD, 0, NULL },
  { "textstyle",            BT_CMD_STYLE_SWITCH, 0, BT_ATOM_ORD, 1, NULL },
  { "scriptstyle",          BT_CMD_STYLE_SWITCH, 0, BT_ATOM_ORD, 2, NULL },
  { "scriptscriptstyle",    BT_CMD_STYLE_SWITCH, 0, BT_ATOM_ORD, 3, NULL },
  { "limits",               BT_CMD_LIMITS_MOD,   0, BT_ATOM_ORD, 1, NULL },
  { "nolimits",             BT_CMD_LIMITS_MOD,   0, BT_ATOM_ORD, 0, NULL },

  /* ---- Accents ---- */
  { "bar",              BT_CMD_ACCENT, 0x0304, BT_ATOM_ORD, BT_ACCENT_BAR, NULL },
  { "hat",              BT_CMD_ACCENT, 0x0302, BT_ATOM_ORD, BT_ACCENT_HAT, NULL },
  { "tilde",            BT_CMD_ACCENT, 0x0303, BT_ATOM_ORD, BT_ACCENT_TILDE, NULL },
  { "vec",              BT_CMD_ACCENT, 0x20D7, BT_ATOM_ORD, BT_ACCENT_VEC, NULL },
  { "dot",              BT_CMD_ACCENT, 0x0307, BT_ATOM_ORD, BT_ACCENT_DOT, NULL },
  { "ddot",             BT_CMD_ACCENT, 0x0308, BT_ATOM_ORD, BT_ACCENT_DDOT, NULL },
  { "breve",            BT_CMD_ACCENT, 0x0306, BT_ATOM_ORD, BT_ACCENT_BREVE, NULL },
  { "check",            BT_CMD_ACCENT, 0x030C, BT_ATOM_ORD, BT_ACCENT_CHECK, NULL },
  { "acute",            BT_CMD_ACCENT, 0x0301, BT_ATOM_ORD, BT_ACCENT_ACUTE, NULL },
  { "grave",            BT_CMD_ACCENT, 0x0300, BT_ATOM_ORD, BT_ACCENT_GRAVE, NULL },
  { "overline",         BT_CMD_ACCENT, 0x0305, BT_ATOM_ORD, BT_ACCENT_OVERLINE, NULL },
  { "underline",        BT_CMD_ACCENT, 0x0332, BT_ATOM_ORD, BT_ACCENT_UNDERLINE, NULL },
  { "widehat",          BT_CMD_ACCENT, 0x0302, BT_ATOM_ORD, BT_ACCENT_WIDEHAT, NULL },
  { "widetilde",        BT_CMD_ACCENT, 0x0303, BT_ATOM_ORD, BT_ACCENT_WIDETILDE, NULL },
  { "overbrace",        BT_CMD_ACCENT, 0x23DE, BT_ATOM_OP,  BT_ACCENT_OVERBRACE, NULL },
  { "underbrace",       BT_CMD_ACCENT, 0x23DF, BT_ATOM_OP,  BT_ACCENT_UNDERBRACE, NULL },
  { "overrightarrow",   BT_CMD_ACCENT, 0x20D7, BT_ATOM_ORD, BT_ACCENT_OVERRIGHTARROW, NULL },

  /* ---- Fonts & text ---- */
  { "mathrm",           BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_ROMAN, NULL },
  { "mathbf",           BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_BOLD, NULL },
  { "mathit",           BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_ITALIC, NULL },
  { "mathbb",           BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_BB, NULL },
  { "mathcal",          BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_CAL, NULL },
  { "mathscr",          BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_CAL, NULL },
  { "mathfrak",         BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_FRAK, NULL },
  { "mathsf",           BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_SANS, NULL },
  { "mathtt",           BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_MONO, NULL },
  { "boldsymbol",       BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_BOLD, NULL },
  { "bm",               BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_BOLD, NULL },
  { "bf",               BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_BOLD, NULL },
  { "rm",               BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_ROMAN, NULL },
  { "it",               BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_ITALIC, NULL },
  { "cal",              BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_CAL, NULL },
  { "Bbb",              BT_CMD_FONT, 0, BT_ATOM_ORD, BT_MATH_FONT_BB, NULL },
  { "text",             BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_TEXT, NULL },
  { "textrm",           BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_TEXT, NULL },
  { "textbf",           BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_BOLD, NULL },
  { "textit",           BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_ITALIC, NULL },
  { "texttt",           BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_MONO, NULL },
  { "textsf",           BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_SANS, NULL },
  { "mbox",             BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_TEXT, NULL },
  { "hbox",             BT_CMD_TEXT, 0, BT_ATOM_ORD, BT_MATH_FONT_TEXT, NULL },
  { "operatorname",     BT_CMD_OPERATORNAME, 0, BT_ATOM_OP, 0, NULL },

  /* ---- Spacing (aux = space in mu, where 18 mu = 1 em) ---- */
  { ",",                BT_CMD_SPACE, 0, BT_ATOM_ORD,  3, NULL },
  { "thinspace",        BT_CMD_SPACE, 0, BT_ATOM_ORD,  3, NULL },
  { ":",                BT_CMD_SPACE, 0, BT_ATOM_ORD,  4, NULL },
  { ">",                BT_CMD_SPACE, 0, BT_ATOM_ORD,  4, NULL },
  { "medspace",         BT_CMD_SPACE, 0, BT_ATOM_ORD,  4, NULL },
  { ";",                BT_CMD_SPACE, 0, BT_ATOM_ORD,  5, NULL },
  { "thickspace",       BT_CMD_SPACE, 0, BT_ATOM_ORD,  5, NULL },
  { "!",                BT_CMD_SPACE, 0, BT_ATOM_ORD, -3, NULL },
  { "negthinspace",     BT_CMD_SPACE, 0, BT_ATOM_ORD, -3, NULL },
  { " ",                BT_CMD_SPACE, 0, BT_ATOM_ORD,  6, NULL },
  { "enspace",          BT_CMD_SPACE, 0, BT_ATOM_ORD,  9, NULL },
  { "quad",             BT_CMD_SPACE, 0, BT_ATOM_ORD, 18, NULL },
  { "qquad",            BT_CMD_SPACE, 0, BT_ATOM_ORD, 36, NULL },

  /* ---- Large operators (Unicode 0x220F..0x2A0C) ---- */
  { "sum",              BT_CMD_LARGE_OP, 0x2211, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "prod",             BT_CMD_LARGE_OP, 0x220F, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "coprod",           BT_CMD_LARGE_OP, 0x2210, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "int",              BT_CMD_LARGE_OP, 0x222B, BT_ATOM_OP, OP_FIXED_LIMITS, NULL },
  { "iint",             BT_CMD_LARGE_OP, 0x222C, BT_ATOM_OP, OP_FIXED_LIMITS, NULL },
  { "iiint",            BT_CMD_LARGE_OP, 0x222D, BT_ATOM_OP, OP_FIXED_LIMITS, NULL },
  { "oint",             BT_CMD_LARGE_OP, 0x222E, BT_ATOM_OP, OP_FIXED_LIMITS, NULL },
  { "bigcup",           BT_CMD_LARGE_OP, 0x22C3, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigcap",           BT_CMD_LARGE_OP, 0x22C2, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigoplus",         BT_CMD_LARGE_OP, 0x2A01, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigotimes",        BT_CMD_LARGE_OP, 0x2A02, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigodot",          BT_CMD_LARGE_OP, 0x2A00, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigvee",           BT_CMD_LARGE_OP, 0x22C1, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigwedge",         BT_CMD_LARGE_OP, 0x22C0, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "bigsqcup",         BT_CMD_LARGE_OP, 0x2A06, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },
  { "biguplus",         BT_CMD_LARGE_OP, 0x2A04, BT_ATOM_OP, OP_MOVABLE_LIMITS, NULL },

  /* ---- Named operators (roman upright text, optional movable limits) ---- */
  { "lim",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "lim" },
  { "limsup",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "lim sup" },
  { "liminf",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "lim inf" },
  { "max",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "max" },
  { "min",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "min" },
  { "sup",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "sup" },
  { "inf",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "inf" },
  { "det",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "det" },
  { "Pr",               BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "Pr" },
  { "gcd",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "gcd" },
  { "arg",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "arg" },
  { "argmax",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "arg max" },
  { "argmin",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_MOVABLE_LIMITS, "arg min" },
  { "sin",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "sin" },
  { "cos",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "cos" },
  { "tan",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "tan" },
  { "cot",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "cot" },
  { "sec",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "sec" },
  { "csc",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "csc" },
  { "arcsin",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "arcsin" },
  { "arccos",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "arccos" },
  { "arctan",           BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "arctan" },
  { "sinh",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "sinh" },
  { "cosh",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "cosh" },
  { "tanh",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "tanh" },
  { "coth",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "coth" },
  { "log",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "log" },
  { "ln",               BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "ln" },
  { "lg",               BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "lg" },
  { "exp",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "exp" },
  { "deg",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "deg" },
  { "dim",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "dim" },
  { "ker",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "ker" },
  { "hom",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "hom" },
  { "rank",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "rank" },
  { "tr",               BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "tr" },
  { "diag",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "diag" },
  { "sgn",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "sgn" },
  { "sign",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "sign" },
  { "Var",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Var" },
  { "var",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "var" },
  { "Cov",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Cov" },
  { "cov",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "cov" },
  { "Corr",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Corr" },
  { "corr",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "corr" },
  { "SE",               BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "SE" },
  { "MSE",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "MSE" },
  { "Bias",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Bias" },
  { "bias",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "bias" },
  { "Exp",              BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Exp" },
  { "Poisson",          BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Poisson" },
  { "Binom",            BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Binom" },
  { "Bern",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Bern" },
  { "Unif",             BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "Unif" },
  { "KL",               BT_CMD_NAMED_OP, 0, BT_ATOM_OP, OP_FIXED_LIMITS,   "KL" },

  /* ---- Greek lowercase (Mathematical Italic Greek in STIX Two Math: U+1D6FC..U+1D71B,
   *      or standard Greek U+03B1..U+03C9 which STIX Two Math also maps cleanly) ---- */
  { "alpha",            BT_CMD_SYMBOL, 0x1D6FC, BT_ATOM_ORD, 0, NULL },
  { "beta",             BT_CMD_SYMBOL, 0x1D6FD, BT_ATOM_ORD, 0, NULL },
  { "gamma",            BT_CMD_SYMBOL, 0x1D6FE, BT_ATOM_ORD, 0, NULL },
  { "delta",            BT_CMD_SYMBOL, 0x1D6FF, BT_ATOM_ORD, 0, NULL },
  { "epsilon",          BT_CMD_SYMBOL, 0x1D716, BT_ATOM_ORD, 0, NULL }, /* lunate epsilon \epsilon */
  { "varepsilon",       BT_CMD_SYMBOL, 0x1D700, BT_ATOM_ORD, 0, NULL },
  { "zeta",             BT_CMD_SYMBOL, 0x1D701, BT_ATOM_ORD, 0, NULL },
  { "eta",              BT_CMD_SYMBOL, 0x1D702, BT_ATOM_ORD, 0, NULL },
  { "theta",            BT_CMD_SYMBOL, 0x1D703, BT_ATOM_ORD, 0, NULL },
  { "vartheta",         BT_CMD_SYMBOL, 0x1D717, BT_ATOM_ORD, 0, NULL },
  { "iota",             BT_CMD_SYMBOL, 0x1D704, BT_ATOM_ORD, 0, NULL },
  { "kappa",            BT_CMD_SYMBOL, 0x1D705, BT_ATOM_ORD, 0, NULL },
  { "varkappa",         BT_CMD_SYMBOL, 0x1D718, BT_ATOM_ORD, 0, NULL },
  { "lambda",           BT_CMD_SYMBOL, 0x1D706, BT_ATOM_ORD, 0, NULL },
  { "mu",               BT_CMD_SYMBOL, 0x1D707, BT_ATOM_ORD, 0, NULL },
  { "nu",               BT_CMD_SYMBOL, 0x1D708, BT_ATOM_ORD, 0, NULL },
  { "xi",               BT_CMD_SYMBOL, 0x1D709, BT_ATOM_ORD, 0, NULL },
  { "pi",               BT_CMD_SYMBOL, 0x1D70B, BT_ATOM_ORD, 0, NULL },
  { "varpi",            BT_CMD_SYMBOL, 0x1D71B, BT_ATOM_ORD, 0, NULL },
  { "rho",              BT_CMD_SYMBOL, 0x1D70C, BT_ATOM_ORD, 0, NULL },
  { "varrho",           BT_CMD_SYMBOL, 0x1D71A, BT_ATOM_ORD, 0, NULL },
  { "sigma",            BT_CMD_SYMBOL, 0x1D70E, BT_ATOM_ORD, 0, NULL },
  { "varsigma",         BT_CMD_SYMBOL, 0x1D70D, BT_ATOM_ORD, 0, NULL },
  { "tau",              BT_CMD_SYMBOL, 0x1D70F, BT_ATOM_ORD, 0, NULL },
  { "upsilon",          BT_CMD_SYMBOL, 0x1D710, BT_ATOM_ORD, 0, NULL },
  { "phi",              BT_CMD_SYMBOL, 0x1D719, BT_ATOM_ORD, 0, NULL }, /* straight phi */
  { "varphi",           BT_CMD_SYMBOL, 0x1D711, BT_ATOM_ORD, 0, NULL }, /* curly phi */
  { "chi",              BT_CMD_SYMBOL, 0x1D712, BT_ATOM_ORD, 0, NULL },
  { "psi",              BT_CMD_SYMBOL, 0x1D713, BT_ATOM_ORD, 0, NULL },
  { "omega",            BT_CMD_SYMBOL, 0x1D714, BT_ATOM_ORD, 0, NULL },

  /* ---- Greek uppercase (upright by TeX convention: U+0393..U+03A9) ---- */
  { "Gamma",            BT_CMD_SYMBOL, 0x0393, BT_ATOM_ORD, 0, NULL },
  { "Delta",            BT_CMD_SYMBOL, 0x0394, BT_ATOM_ORD, 0, NULL },
  { "Theta",            BT_CMD_SYMBOL, 0x0398, BT_ATOM_ORD, 0, NULL },
  { "Lambda",           BT_CMD_SYMBOL, 0x039B, BT_ATOM_ORD, 0, NULL },
  { "Xi",               BT_CMD_SYMBOL, 0x039E, BT_ATOM_ORD, 0, NULL },
  { "Pi",               BT_CMD_SYMBOL, 0x03A0, BT_ATOM_ORD, 0, NULL },
  { "Sigma",            BT_CMD_SYMBOL, 0x03A3, BT_ATOM_ORD, 0, NULL },
  { "Upsilon",          BT_CMD_SYMBOL, 0x03A5, BT_ATOM_ORD, 0, NULL },
  { "Phi",              BT_CMD_SYMBOL, 0x03A6, BT_ATOM_ORD, 0, NULL },
  { "Psi",              BT_CMD_SYMBOL, 0x03A8, BT_ATOM_ORD, 0, NULL },
  { "Omega",            BT_CMD_SYMBOL, 0x03A9, BT_ATOM_ORD, 0, NULL },

  /* ---- Escaped characters & miscellaneous symbols ---- */
  { "{",                BT_CMD_SYMBOL, 0x007B, BT_ATOM_OPEN,  0, NULL },
  { "}",                BT_CMD_SYMBOL, 0x007D, BT_ATOM_CLOSE, 0, NULL },
  { "lbrace",           BT_CMD_SYMBOL, 0x007B, BT_ATOM_OPEN,  0, NULL },
  { "rbrace",           BT_CMD_SYMBOL, 0x007D, BT_ATOM_CLOSE, 0, NULL },
  { "lbrack",           BT_CMD_SYMBOL, 0x005B, BT_ATOM_OPEN,  0, NULL },
  { "rbrack",           BT_CMD_SYMBOL, 0x005D, BT_ATOM_CLOSE, 0, NULL },
  { "|",                BT_CMD_SYMBOL, 0x2016, BT_ATOM_ORD,   0, NULL },
  { "Vert",             BT_CMD_SYMBOL, 0x2016, BT_ATOM_ORD,   0, NULL },
  { "vert",             BT_CMD_SYMBOL, 0x007C, BT_ATOM_ORD,   0, NULL },
  { "langle",           BT_CMD_SYMBOL, 0x27E8, BT_ATOM_OPEN,  0, NULL },
  { "rangle",           BT_CMD_SYMBOL, 0x27E9, BT_ATOM_CLOSE, 0, NULL },
  { "lfloor",           BT_CMD_SYMBOL, 0x230A, BT_ATOM_OPEN,  0, NULL },
  { "rfloor",           BT_CMD_SYMBOL, 0x230B, BT_ATOM_CLOSE, 0, NULL },
  { "lceil",            BT_CMD_SYMBOL, 0x2308, BT_ATOM_OPEN,  0, NULL },
  { "rceil",            BT_CMD_SYMBOL, 0x2309, BT_ATOM_CLOSE, 0, NULL },
  { "%",                BT_CMD_SYMBOL, 0x0025, BT_ATOM_ORD,   0, NULL },
  { "$",                BT_CMD_SYMBOL, 0x0024, BT_ATOM_ORD,   0, NULL },
  { "#",                BT_CMD_SYMBOL, 0x0023, BT_ATOM_ORD,   0, NULL },
  { "&",                BT_CMD_SYMBOL, 0x0026, BT_ATOM_ORD,   0, NULL },
  { "_",                BT_CMD_SYMBOL, 0x005F, BT_ATOM_ORD,   0, NULL },
  { "infty",            BT_CMD_SYMBOL, 0x221E, BT_ATOM_ORD,   0, NULL },
  { "partial",          BT_CMD_SYMBOL, 0x2202, BT_ATOM_ORD,   0, NULL },
  { "nabla",            BT_CMD_SYMBOL, 0x2207, BT_ATOM_ORD,   0, NULL },
  { "forall",           BT_CMD_SYMBOL, 0x2200, BT_ATOM_ORD,   0, NULL },
  { "exists",           BT_CMD_SYMBOL, 0x2203, BT_ATOM_ORD,   0, NULL },
  { "nexists",          BT_CMD_SYMBOL, 0x2204, BT_ATOM_ORD,   0, NULL },
  { "emptyset",         BT_CMD_SYMBOL, 0x2205, BT_ATOM_ORD,   0, NULL },
  { "varnothing",       BT_CMD_SYMBOL, 0x2205, BT_ATOM_ORD,   0, NULL },
  { "ell",              BT_CMD_SYMBOL, 0x2113, BT_ATOM_ORD,   0, NULL },
  { "hbar",             BT_CMD_SYMBOL, 0x210F, BT_ATOM_ORD,   0, NULL },
  { "Re",               BT_CMD_SYMBOL, 0x211C, BT_ATOM_ORD,   0, NULL },
  { "Im",               BT_CMD_SYMBOL, 0x2111, BT_ATOM_ORD,   0, NULL },
  { "wp",               BT_CMD_SYMBOL, 0x2118, BT_ATOM_ORD,   0, NULL },
  { "aleph",            BT_CMD_SYMBOL, 0x2135, BT_ATOM_ORD,   0, NULL },
  { "imath",            BT_CMD_SYMBOL, 0x1D6A4, BT_ATOM_ORD,  0, NULL },
  { "jmath",            BT_CMD_SYMBOL, 0x1D6A5, BT_ATOM_ORD,  0, NULL },
  { "prime",            BT_CMD_SYMBOL, 0x2032, BT_ATOM_ORD,   0, NULL },
  { "neg",              BT_CMD_SYMBOL, 0x00AC, BT_ATOM_ORD,   0, NULL },
  { "lnot",             BT_CMD_SYMBOL, 0x00AC, BT_ATOM_ORD,   0, NULL },
  { "angle",            BT_CMD_SYMBOL, 0x2220, BT_ATOM_ORD,   0, NULL },
  { "triangle",         BT_CMD_SYMBOL, 0x25B3, BT_ATOM_ORD,   0, NULL },
  { "checkmark",        BT_CMD_SYMBOL, 0x2713, BT_ATOM_ORD,   0, NULL },
  { "dagger",           BT_CMD_SYMBOL, 0x2020, BT_ATOM_BIN,   0, NULL },
  { "ddagger",          BT_CMD_SYMBOL, 0x2021, BT_ATOM_BIN,   0, NULL },
  { "top",              BT_CMD_SYMBOL, 0x22A4, BT_ATOM_ORD,   0, NULL },
  { "bot",              BT_CMD_SYMBOL, 0x22A5, BT_ATOM_ORD,   0, NULL },
  { "dots",             BT_CMD_SYMBOL, 0x2026, BT_ATOM_INNER, 0, NULL },
  { "ldots",            BT_CMD_SYMBOL, 0x2026, BT_ATOM_INNER, 0, NULL },
  { "cdots",            BT_CMD_SYMBOL, 0x22EF, BT_ATOM_INNER, 0, NULL },
  { "vdots",            BT_CMD_SYMBOL, 0x22EE, BT_ATOM_ORD,   0, NULL },
  { "ddots",            BT_CMD_SYMBOL, 0x22F1, BT_ATOM_ORD,   0, NULL },
  { "colon",            BT_CMD_SYMBOL, 0x003A, BT_ATOM_PUNCT, 0, NULL },

  /* ---- Binary operators ---- */
  { "pm",               BT_CMD_SYMBOL, 0x00B1, BT_ATOM_BIN, 0, NULL },
  { "mp",               BT_CMD_SYMBOL, 0x2213, BT_ATOM_BIN, 0, NULL },
  { "times",            BT_CMD_SYMBOL, 0x00D7, BT_ATOM_BIN, 0, NULL },
  { "div",              BT_CMD_SYMBOL, 0x00F7, BT_ATOM_BIN, 0, NULL },
  { "cdot",             BT_CMD_SYMBOL, 0x22C5, BT_ATOM_BIN, 0, NULL },
  { "cdotp",            BT_CMD_SYMBOL, 0x22C5, BT_ATOM_PUNCT, 0, NULL },
  { "ast",              BT_CMD_SYMBOL, 0x2217, BT_ATOM_BIN, 0, NULL },
  { "star",             BT_CMD_SYMBOL, 0x22C6, BT_ATOM_BIN, 0, NULL },
  { "circ",             BT_CMD_SYMBOL, 0x2218, BT_ATOM_BIN, 0, NULL },
  { "bullet",           BT_CMD_SYMBOL, 0x2219, BT_ATOM_BIN, 0, NULL },
  { "oplus",            BT_CMD_SYMBOL, 0x2295, BT_ATOM_BIN, 0, NULL },
  { "ominus",           BT_CMD_SYMBOL, 0x2296, BT_ATOM_BIN, 0, NULL },
  { "otimes",           BT_CMD_SYMBOL, 0x2297, BT_ATOM_BIN, 0, NULL },
  { "oslash",           BT_CMD_SYMBOL, 0x2298, BT_ATOM_BIN, 0, NULL },
  { "odot",             BT_CMD_SYMBOL, 0x2299, BT_ATOM_BIN, 0, NULL },
  { "cup",              BT_CMD_SYMBOL, 0x222A, BT_ATOM_BIN, 0, NULL },
  { "cap",              BT_CMD_SYMBOL, 0x2229, BT_ATOM_BIN, 0, NULL },
  { "sqcup",            BT_CMD_SYMBOL, 0x2294, BT_ATOM_BIN, 0, NULL },
  { "sqcap",            BT_CMD_SYMBOL, 0x2293, BT_ATOM_BIN, 0, NULL },
  { "uplus",            BT_CMD_SYMBOL, 0x228E, BT_ATOM_BIN, 0, NULL },
  { "vee",              BT_CMD_SYMBOL, 0x2228, BT_ATOM_BIN, 0, NULL },
  { "lor",              BT_CMD_SYMBOL, 0x2228, BT_ATOM_BIN, 0, NULL },
  { "wedge",            BT_CMD_SYMBOL, 0x2227, BT_ATOM_BIN, 0, NULL },
  { "land",             BT_CMD_SYMBOL, 0x2227, BT_ATOM_BIN, 0, NULL },
  { "setminus",         BT_CMD_SYMBOL, 0x2216, BT_ATOM_BIN, 0, NULL },
  { "backslash",        BT_CMD_SYMBOL, 0x005C, BT_ATOM_ORD, 0, NULL },
  { "wr",               BT_CMD_SYMBOL, 0x2240, BT_ATOM_BIN, 0, NULL },
  { "amalg",            BT_CMD_SYMBOL, 0x2A3F, BT_ATOM_BIN, 0, NULL },
  { "triangleleft",     BT_CMD_SYMBOL, 0x25C1, BT_ATOM_BIN, 0, NULL },
  { "triangleright",    BT_CMD_SYMBOL, 0x25B7, BT_ATOM_BIN, 0, NULL },

  /* ---- Relations ---- */
  { "le",               BT_CMD_SYMBOL, 0x2264, BT_ATOM_REL, 0, NULL },
  { "leq",              BT_CMD_SYMBOL, 0x2264, BT_ATOM_REL, 0, NULL },
  { "leqslant",         BT_CMD_SYMBOL, 0x2A7D, BT_ATOM_REL, 0, NULL },
  { "ge",               BT_CMD_SYMBOL, 0x2265, BT_ATOM_REL, 0, NULL },
  { "geq",              BT_CMD_SYMBOL, 0x2265, BT_ATOM_REL, 0, NULL },
  { "geqslant",         BT_CMD_SYMBOL, 0x2A7E, BT_ATOM_REL, 0, NULL },
  { "ne",               BT_CMD_SYMBOL, 0x2260, BT_ATOM_REL, 0, NULL },
  { "neq",              BT_CMD_SYMBOL, 0x2260, BT_ATOM_REL, 0, NULL },
  { "ll",               BT_CMD_SYMBOL, 0x226A, BT_ATOM_REL, 0, NULL },
  { "gg",               BT_CMD_SYMBOL, 0x226B, BT_ATOM_REL, 0, NULL },
  { "prec",             BT_CMD_SYMBOL, 0x227A, BT_ATOM_REL, 0, NULL },
  { "succ",             BT_CMD_SYMBOL, 0x227B, BT_ATOM_REL, 0, NULL },
  { "preceq",           BT_CMD_SYMBOL, 0x2AAF, BT_ATOM_REL, 0, NULL },
  { "succeq",           BT_CMD_SYMBOL, 0x2AB0, BT_ATOM_REL, 0, NULL },
  { "approx",           BT_CMD_SYMBOL, 0x2248, BT_ATOM_REL, 0, NULL },
  { "sim",              BT_CMD_SYMBOL, 0x223C, BT_ATOM_REL, 0, NULL },
  { "simeq",            BT_CMD_SYMBOL, 0x2243, BT_ATOM_REL, 0, NULL },
  { "cong",             BT_CMD_SYMBOL, 0x2245, BT_ATOM_REL, 0, NULL },
  { "equiv",            BT_CMD_SYMBOL, 0x2261, BT_ATOM_REL, 0, NULL },
  { "propto",           BT_CMD_SYMBOL, 0x221D, BT_ATOM_REL, 0, NULL },
  { "asymp",            BT_CMD_SYMBOL, 0x224D, BT_ATOM_REL, 0, NULL },
  { "doteq",            BT_CMD_SYMBOL, 0x2250, BT_ATOM_REL, 0, NULL },
  { "triangleq",        BT_CMD_SYMBOL, 0x225C, BT_ATOM_REL, 0, NULL },
  { "in",               BT_CMD_SYMBOL, 0x2208, BT_ATOM_REL, 0, NULL },
  { "notin",            BT_CMD_SYMBOL, 0x2209, BT_ATOM_REL, 0, NULL },
  { "ni",               BT_CMD_SYMBOL, 0x220B, BT_ATOM_REL, 0, NULL },
  { "owns",             BT_CMD_SYMBOL, 0x220B, BT_ATOM_REL, 0, NULL },
  { "subset",           BT_CMD_SYMBOL, 0x2282, BT_ATOM_REL, 0, NULL },
  { "supset",           BT_CMD_SYMBOL, 0x2283, BT_ATOM_REL, 0, NULL },
  { "subseteq",         BT_CMD_SYMBOL, 0x2286, BT_ATOM_REL, 0, NULL },
  { "supseteq",         BT_CMD_SYMBOL, 0x2287, BT_ATOM_REL, 0, NULL },
  { "nsubseteq",        BT_CMD_SYMBOL, 0x2288, BT_ATOM_REL, 0, NULL },
  { "subsetneq",        BT_CMD_SYMBOL, 0x228A, BT_ATOM_REL, 0, NULL },
  { "sqsubseteq",       BT_CMD_SYMBOL, 0x2291, BT_ATOM_REL, 0, NULL },
  { "sqsupseteq",       BT_CMD_SYMBOL, 0x2292, BT_ATOM_REL, 0, NULL },
  { "perp",             BT_CMD_SYMBOL, 0x22A5, BT_ATOM_REL, 0, NULL },
  { "parallel",         BT_CMD_SYMBOL, 0x2225, BT_ATOM_REL, 0, NULL },
  { "mid",              BT_CMD_SYMBOL, 0x2223, BT_ATOM_REL, 0, NULL },
  { "nmid",             BT_CMD_SYMBOL, 0x2224, BT_ATOM_REL, 0, NULL },
  { "vdash",            BT_CMD_SYMBOL, 0x22A2, BT_ATOM_REL, 0, NULL },
  { "dashv",            BT_CMD_SYMBOL, 0x22A3, BT_ATOM_REL, 0, NULL },
  { "models",           BT_CMD_SYMBOL, 0x22A8, BT_ATOM_REL, 0, NULL },
  { "iid",              BT_CMD_NAMED_OP, 0, BT_ATOM_REL, OP_FIXED_LIMITS, "i.i.d." },
  { "indep",            BT_CMD_SYMBOL, 0x2AEB, BT_ATOM_REL, 0, NULL },

  /* ---- Arrows ---- */
  { "to",               BT_CMD_SYMBOL, 0x2192, BT_ATOM_REL, 0, NULL },
  { "rightarrow",       BT_CMD_SYMBOL, 0x2192, BT_ATOM_REL, 0, NULL },
  { "leftarrow",        BT_CMD_SYMBOL, 0x2190, BT_ATOM_REL, 0, NULL },
  { "gets",             BT_CMD_SYMBOL, 0x2190, BT_ATOM_REL, 0, NULL },
  { "leftrightarrow",   BT_CMD_SYMBOL, 0x2194, BT_ATOM_REL, 0, NULL },
  { "uparrow",          BT_CMD_SYMBOL, 0x2191, BT_ATOM_REL, 0, NULL },
  { "downarrow",        BT_CMD_SYMBOL, 0x2193, BT_ATOM_REL, 0, NULL },
  { "updownarrow",      BT_CMD_SYMBOL, 0x2195, BT_ATOM_REL, 0, NULL },
  { "Rightarrow",       BT_CMD_SYMBOL, 0x21D2, BT_ATOM_REL, 0, NULL },
  { "Leftarrow",        BT_CMD_SYMBOL, 0x21D0, BT_ATOM_REL, 0, NULL },
  { "Leftrightarrow",   BT_CMD_SYMBOL, 0x21D4, BT_ATOM_REL, 0, NULL },
  { "implies",          BT_CMD_SYMBOL, 0x27F9, BT_ATOM_REL, 0, NULL },
  { "impliedby",        BT_CMD_SYMBOL, 0x27F8, BT_ATOM_REL, 0, NULL },
  { "iff",              BT_CMD_SYMBOL, 0x27FA, BT_ATOM_REL, 0, NULL },
  { "mapsto",           BT_CMD_SYMBOL, 0x21A6, BT_ATOM_REL, 0, NULL },
  { "longmapsto",       BT_CMD_SYMBOL, 0x27FC, BT_ATOM_REL, 0, NULL },
  { "longrightarrow",   BT_CMD_SYMBOL, 0x27F6, BT_ATOM_REL, 0, NULL },
  { "longleftarrow",    BT_CMD_SYMBOL, 0x27F5, BT_ATOM_REL, 0, NULL },
  { "longleftrightarrow", BT_CMD_SYMBOL, 0x27F7, BT_ATOM_REL, 0, NULL },
  { "Longrightarrow",   BT_CMD_SYMBOL, 0x27F9, BT_ATOM_REL, 0, NULL },
  { "Longleftarrow",    BT_CMD_SYMBOL, 0x27F8, BT_ATOM_REL, 0, NULL },
  { "Longleftrightarrow", BT_CMD_SYMBOL, 0x27FA, BT_ATOM_REL, 0, NULL },
  { "hookrightarrow",   BT_CMD_SYMBOL, 0x21AA, BT_ATOM_REL, 0, NULL },
  { "hookleftarrow",    BT_CMD_SYMBOL, 0x21A9, BT_ATOM_REL, 0, NULL },
  { "nearrow",          BT_CMD_SYMBOL, 0x2197, BT_ATOM_REL, 0, NULL },
  { "searrow",          BT_CMD_SYMBOL, 0x2198, BT_ATOM_REL, 0, NULL },
  { "swarrow",          BT_CMD_SYMBOL, 0x2199, BT_ATOM_REL, 0, NULL },
  { "nwarrow",          BT_CMD_SYMBOL, 0x2196, BT_ATOM_REL, 0, NULL },
  { "rightharpoonup",   BT_CMD_SYMBOL, 0x21C0, BT_ATOM_REL, 0, NULL },
  { "rightharpoondown", BT_CMD_SYMBOL, 0x21C1, BT_ATOM_REL, 0, NULL },
  { "leftharpoonup",    BT_CMD_SYMBOL, 0x21BC, BT_ATOM_REL, 0, NULL },
  { "leftharpoondown",  BT_CMD_SYMBOL, 0x21BD, BT_ATOM_REL, 0, NULL },
  { "rightleftharpoons", BT_CMD_SYMBOL, 0x21CC, BT_ATOM_REL, 0, NULL },
};

const BtMathCmdEntry *bt_math_cmd_lookup(const char *name, gsize name_len)
{
  if (name == NULL || name_len == 0)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS(k_cmds); i++) {
    if (strlen(k_cmds[i].name) == name_len &&
        memcmp(k_cmds[i].name, name, name_len) == 0)
      return &k_cmds[i];
  }
  return NULL;
}

gunichar bt_math_map_styled_char(gunichar ch, BtMathFontStyle style, gboolean in_math_mode)
{
  if (!in_math_mode || style == BT_MATH_FONT_TEXT || style == BT_MATH_FONT_ROMAN)
    return ch;

  /* Default math mode: ASCII letters a-z A-Z become Mathematical Italic */
  if (style == BT_MATH_FONT_NORMAL || style == BT_MATH_FONT_ITALIC) {
    if (ch >= 'A' && ch <= 'Z')
      return 0x1D434u + (ch - 'A');
    if (ch >= 'a' && ch <= 'z') {
      if (ch == 'h')
        return 0x210Eu; /* Planck constant italic h in Unicode BMP */
      return 0x1D44Eu + (ch - 'a');
    }
    return ch;
  }

  if (style == BT_MATH_FONT_BOLD) {
    if (ch >= 'A' && ch <= 'Z')
      return 0x1D400u + (ch - 'A');
    if (ch >= 'a' && ch <= 'z')
      return 0x1D41Au + (ch - 'a');
    if (ch >= '0' && ch <= '9')
      return 0x1D7CEu + (ch - '0');
    /* Bold Greek italic lowercase U+1D6FC..U+1D71B -> U+1D736..U+1D755 */
    if (ch >= 0x1D6FCu && ch <= 0x1D71Bu)
      return 0x1D736u + (ch - 0x1D6FCu);
    /* Bold Greek uppercase U+0391..U+03A9 -> U+1D6A8..U+1D6C0 */
    if (ch >= 0x0391u && ch <= 0x03A9u)
      return 0x1D6A8u + (ch - 0x0391u);
    return ch;
  }

  if (style == BT_MATH_FONT_BB) {
    if (ch >= 'A' && ch <= 'Z') {
      switch (ch) {
        case 'C': return 0x2102u;
        case 'H': return 0x210Du;
        case 'N': return 0x2115u;
        case 'P': return 0x2119u;
        case 'Q': return 0x211Au;
        case 'R': return 0x211Du;
        case 'Z': return 0x2124u;
        default:  return 0x1D538u + (ch - 'A');
      }
    }
    if (ch >= 'a' && ch <= 'z')
      return 0x1D552u + (ch - 'a');
    if (ch >= '0' && ch <= '9')
      return 0x1D7D8u + (ch - '0');
    return ch;
  }

  if (style == BT_MATH_FONT_CAL) {
    if (ch >= 'A' && ch <= 'Z') {
      switch (ch) {
        case 'B': return 0x212Cu;
        case 'E': return 0x2130u;
        case 'F': return 0x2131u;
        case 'H': return 0x210Bu;
        case 'I': return 0x2110u;
        case 'L': return 0x2112u;
        case 'M': return 0x2133u;
        case 'R': return 0x211Bu;
        default:  return 0x1D49Cu + (ch - 'A');
      }
    }
    if (ch >= 'a' && ch <= 'z') {
      switch (ch) {
        case 'e': return 0x212Fu;
        case 'g': return 0x210Au;
        case 'o': return 0x2134u;
        default:  return 0x1D4B6u + (ch - 'a');
      }
    }
    return ch;
  }

  if (style == BT_MATH_FONT_FRAK) {
    if (ch >= 'A' && ch <= 'Z') {
      switch (ch) {
        case 'C': return 0x212Du;
        case 'H': return 0x210Cu;
        case 'I': return 0x2111u;
        case 'R': return 0x211Cu;
        case 'Z': return 0x2128u;
        default:  return 0x1D504u + (ch - 'A');
      }
    }
    if (ch >= 'a' && ch <= 'z')
      return 0x1D51Eu + (ch - 'a');
    return ch;
  }

  if (style == BT_MATH_FONT_SANS) {
    if (ch >= 'A' && ch <= 'Z')
      return 0x1D5A0u + (ch - 'A');
    if (ch >= 'a' && ch <= 'z')
      return 0x1D5BAu + (ch - 'a');
    if (ch >= '0' && ch <= '9')
      return 0x1D7E2u + (ch - '0');
    return ch;
  }

  if (style == BT_MATH_FONT_MONO) {
    if (ch >= 'A' && ch <= 'Z')
      return 0x1D670u + (ch - 'A');
    if (ch >= 'a' && ch <= 'z')
      return 0x1D68Au + (ch - 'a');
    if (ch >= '0' && ch <= '9')
      return 0x1D7F6u + (ch - '0');
    return ch;
  }

  return ch;
}

BtAtomClass bt_math_char_atom_class(gunichar ch)
{
  switch (ch) {
    case '+':
    case '-':
    case 0x2212: /* Unicode minus */
    case '*':
    case 0x00B1:
    case 0x00D7:
    case 0x22C5:
      return BT_ATOM_BIN;
    case '=':
    case '<':
    case '>':
    case ':':
    case 0x2264:
    case 0x2265:
    case 0x2260:
    case 0x2248:
    case 0x223C:
    case 0x2208:
      return BT_ATOM_REL;
    case '(':
    case '[':
    case '{':
    case 0x27E8:
      return BT_ATOM_OPEN;
    case ')':
    case ']':
    case '}':
    case '?':
    case '!':
    case 0x27E9:
      return BT_ATOM_CLOSE;
    case ',':
    case ';':
      return BT_ATOM_PUNCT;
    default:
      return BT_ATOM_ORD;
  }
}

gunichar bt_math_resolve_delimiter(const char *tok, gsize len)
{
  if (tok == NULL || len == 0)
    return 0;
  if (len == 1) {
    switch (tok[0]) {
      case '.': return 0; /* Null delimiter */
      case '(': return '(';
      case ')': return ')';
      case '[': return '[';
      case ']': return ']';
      case '{': return '{';
      case '}': return '}';
      case '|': return '|';
      case '/': return '/';
      case '<': return 0x27E8;
      case '>': return 0x27E9;
      default:  return (gunichar)(unsigned char)tok[0];
    }
  }
  const char *name = (tok[0] == '\\') ? (tok + 1) : tok;
  gsize nlen = (tok[0] == '\\') ? (len - 1) : len;
  if (nlen == 1) {
    if (name[0] == '{') return '{';
    if (name[0] == '}') return '}';
    if (name[0] == '|') return 0x2016; /* \| double vertical bar */
  }
  const BtMathCmdEntry *e = bt_math_cmd_lookup(name, nlen);
  if (e != NULL && e->codepoint != 0)
    return e->codepoint;
  return 0;
}
