/* mathcmds.h — LaTeX command registry, symbol mappings, and atom classes (M2).
 * Pure C17, GLib-only (no GTK, no Cairo). */
#ifndef BT_MATHCMDS_H
#define BT_MATHCMDS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BT_ATOM_ORD   = 0, /* Ordinary: x, 0, \alpha, \partial, \infty */
  BT_ATOM_OP    = 1, /* Large / named operator: \sum, \int, \lim, \sin, \Pr */
  BT_ATOM_BIN   = 2, /* Binary operator: +, -, \times, \cdot, \cup */
  BT_ATOM_REL   = 3, /* Relation: =, <, >, \le, \ge, \sim, \to, \in */
  BT_ATOM_OPEN  = 4, /* Opening delimiter: (, [, \{, \langle */
  BT_ATOM_CLOSE = 5, /* Closing delimiter: ), ], \}, \rangle */
  BT_ATOM_PUNCT = 6, /* Punctuation: , ; : */
  BT_ATOM_INNER = 7, /* Inner subformula: \frac, \left...\right */
} BtAtomClass;

typedef enum {
  BT_MATH_FONT_NORMAL      = 0, /* default: italic for letters, upright for digits */
  BT_MATH_FONT_ROMAN       = 1, /* \mathrm, \operator */
  BT_MATH_FONT_BOLD        = 2, /* \mathbf, \boldsymbol, \bm */
  BT_MATH_FONT_ITALIC      = 3, /* \mathit */
  BT_MATH_FONT_BB          = 4, /* \mathbb */
  BT_MATH_FONT_CAL         = 5, /* \mathcal, \mathscr */
  BT_MATH_FONT_FRAK        = 6, /* \mathfrak */
  BT_MATH_FONT_SANS        = 7, /* \mathsf */
  BT_MATH_FONT_MONO        = 8, /* \mathtt */
  BT_MATH_FONT_TEXT        = 9, /* \text, \textrm, \textbf, \textit */
} BtMathFontStyle;

typedef enum {
  BT_ACCENT_BAR       = 0, /* \bar */
  BT_ACCENT_HAT       = 1, /* \hat */
  BT_ACCENT_TILDE     = 2, /* \tilde */
  BT_ACCENT_VEC       = 3, /* \vec */
  BT_ACCENT_DOT       = 4, /* \dot */
  BT_ACCENT_DDOT      = 5, /* \ddot */
  BT_ACCENT_BREVE     = 6, /* \breve */
  BT_ACCENT_CHECK     = 7, /* \check */
  BT_ACCENT_ACUTE     = 8, /* \acute */
  BT_ACCENT_GRAVE     = 9, /* \grave */
  BT_ACCENT_OVERLINE  = 10, /* \overline (rule) */
  BT_ACCENT_UNDERLINE = 11, /* \underline (rule) */
  BT_ACCENT_WIDEHAT   = 12, /* \widehat */
  BT_ACCENT_WIDETILDE = 13, /* \widetilde */
  BT_ACCENT_OVERBRACE = 14, /* \overbrace */
  BT_ACCENT_UNDERBRACE = 15, /* \underbrace */
  BT_ACCENT_OVERRIGHTARROW = 16, /* \overrightarrow */
} BtAccentKind;

typedef enum {
  BT_CMD_SYMBOL = 0,       /* Maps directly to a Unicode codepoint + BtAtomClass */
  BT_CMD_LARGE_OP,         /* \sum, \prod, \int, etc. with movable/fixed limits */
  BT_CMD_NAMED_OP,         /* \sin, \cos, \lim, \max, \Pr, \Var, \Cov, \arg, etc. */
  BT_CMD_FRAC,             /* \frac, \dfrac, \tfrac, \cfrac */
  BT_CMD_BINOM,            /* \binom, \dbinom, \tbinom */
  BT_CMD_SQRT,             /* \sqrt[n]{x} */
  BT_CMD_ACCENT,           /* \bar, \hat, \tilde, \vec, \dot, \ddot, \overline... */
  BT_CMD_FONT,             /* \mathbf, \mathbb, \mathcal, \mathrm, \mathit, \bm... */
  BT_CMD_TEXT,             /* \text, \textrm, \textbf, \textit, \texttt */
  BT_CMD_OPERATORNAME,     /* \operatorname{...} / \operatorname*{...} */
  BT_CMD_SPACE,            /* \, \; \: \! \quad \qquad \  \enspace \thinspace */
  BT_CMD_LEFT,             /* \left */
  BT_CMD_RIGHT,            /* \right */
  BT_CMD_MIDDLE,           /* \middle */
  BT_CMD_BIG_DELIM,        /* \big, \Big, \bigg, \Bigg (+ l/r/m variants) */
  BT_CMD_STYLE_SWITCH,     /* \displaystyle, \textstyle, \scriptstyle, \scriptscriptstyle */
  BT_CMD_LIMITS_MOD,       /* \limits, \nolimits */
  BT_CMD_BEGIN,            /* \begin{env} */
  BT_CMD_END,              /* \end{env} */
  BT_CMD_OVERSET,          /* \overset{top}{base}, \stackrel{top}{base} */
  BT_CMD_UNDERSET,         /* \underset{bot}{base} */
  BT_CMD_PHANTOM,          /* \phantom, \hphantom, \vphantom */
  BT_CMD_COLOR_NOOP,       /* \color, \textcolor, \boxed (renders argument cleanly) */
  BT_CMD_MOD,              /* \bmod, \pmod, \mod */
  BT_CMD_NOT,              /* \not prefix before relation */
  BT_CMD_FORBIDDEN,        /* Security blocklist: \input, \include, \def, \catcode... */
} BtCmdKind;

typedef struct {
  const char *name;        /* Without leading backslash, e.g. "alpha", "frac" */
  BtCmdKind kind;
  gunichar codepoint;      /* Unicode codepoint for SYMBOL / LARGE_OP / ACCENT */
  BtAtomClass atom_class;  /* Atom class for spacing */
  int aux;                 /* Extra parameter (e.g. BtAccentKind, BtMathFontStyle, movable_limits, space_mu, size_step) */
  const char *op_label;    /* Display string for NAMED_OP (e.g. "lim", "Var", "arg max") */
} BtMathCmdEntry;

/* Looks up a LaTeX command name (without leading backslash, length name_len).
 * Returns NULL if unknown. */
const BtMathCmdEntry *bt_math_cmd_lookup(const char *name, gsize name_len);

/* Maps an ASCII letter/digit + BtMathFontStyle to its Mathematical Alphanumeric
 * Symbols Unicode codepoint (or returns the base codepoint if upright/unmapped). */
gunichar bt_math_map_styled_char(gunichar ch, BtMathFontStyle style, gboolean in_math_mode);

/* Returns default BtAtomClass for an unescaped single ASCII/Unicode character in math mode. */
BtAtomClass bt_math_char_atom_class(gunichar ch);

/* Maps a delimiter token (e.g. "(", ")", "[", "]", "{", "}", "|", "\\|", "\\langle")
 * to its Unicode codepoint, or 0 if it is the empty delimiter "." or unknown. */
gunichar bt_math_resolve_delimiter(const char *tok, gsize len);

G_END_DECLS

#endif /* BT_MATHCMDS_H */
