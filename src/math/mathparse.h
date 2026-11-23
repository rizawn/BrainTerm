/* mathparse.h — Recursive-descent LaTeX math subset parser to arena AST (M2).
 * Pure C17, GLib-only (no GTK, no Cairo). Enforces §B.17 untrusted-input limits. */
#ifndef BT_MATHPARSE_H
#define BT_MATHPARSE_H

#include <glib.h>
#include "mathcmds.h"
#include "mathtok.h"

G_BEGIN_DECLS

#define BT_MATH_MAX_INLINE_BYTES  4096u
#define BT_MATH_MAX_DISPLAY_BYTES 16384u
#define BT_MATH_MAX_DEPTH         64u
#define BT_MATH_MAX_NODES         5000u
#define BT_MATH_MAX_MATRIX_DIM    20u
#define BT_MATH_MAX_ERRORS        64u

typedef enum {
  BT_NODE_ROW = 0,       /* Sequence of sibling nodes */
  BT_NODE_SYMBOL,        /* Single styled Unicode symbol + atom class */
  BT_NODE_TEXT,          /* Upright text run (\text{...}, \operatorname{...}, named op) */
  BT_NODE_SCRIPTS,       /* Base node + optional sub and/or sup */
  BT_NODE_FRAC,          /* Numerator + denominator + bar flag + optional left/right delim (\binom) */
  BT_NODE_SQRT,          /* Radicand + optional degree */
  BT_NODE_ACCENT,        /* Base + BtAccentKind + accent codepoint */
  BT_NODE_DELIM_GROUP,   /* \left ... \right (with optional \middle delimiters inside row) */
  BT_NODE_LARGE_OP,      /* \sum, \int, \lim with movable_limits / limits override */
  BT_NODE_MATRIX,        /* rows x cols grid with optional surrounding delimiters & alignment */
  BT_NODE_SPACE,         /* Explicit horizontal space in mu (18 mu = 1 em) */
  BT_NODE_STYLE_SWITCH,  /* \displaystyle, \textstyle, \scriptstyle, \scriptscriptstyle */
  BT_NODE_OVERUNDER,     /* \overset / \underset */
  BT_NODE_ERROR,         /* Non-fatal parse error placeholder */
} BtMathNodeKind;

typedef enum {
  BT_LIMITS_AUTO   = 0,  /* Use movable_limits in display mode, side scripts in inline */
  BT_LIMITS_ALWAYS = 1,  /* \limits */
  BT_LIMITS_NEVER  = 2,  /* \nolimits */
} BtLimitsMode;

typedef struct BtMathNode BtMathNode;

struct BtMathNode {
  BtMathNodeKind kind;
  BtAtomClass atom_class;
  gsize src_offset;
  gsize src_len;

  union {
    struct {
      BtMathNode **items;
      guint count;
    } row;

    struct {
      gunichar codepoint;
      gunichar base_char;
      BtMathFontStyle font_style;
      int big_step; /* 0 = normal, 1..4 = \big..\Bigg */
    } symbol;

    struct {
      const char *text; /* Arena-allocated UTF-8 string */
      BtMathFontStyle font_style;
      gboolean is_operator;
      gboolean movable_limits;
      BtLimitsMode limits_mode;
    } text;

    struct {
      BtMathNode *base;
      BtMathNode *sub;
      BtMathNode *sup;
    } scripts;

    struct {
      BtMathNode *num;
      BtMathNode *den;
      gboolean has_bar;
      int style_override; /* 0=auto, 1=display (\dfrac), 2=text (\tfrac) */
      gunichar left_delim;
      gunichar right_delim;
    } frac;

    struct {
      BtMathNode *radicand;
      BtMathNode *index; /* NULL for square root */
    } sqrt_node;

    struct {
      BtMathNode *base;
      BtAccentKind accent_kind;
      gunichar accent_cp;
    } accent;

    struct {
      gunichar left_delim;  /* 0 if '.' */
      gunichar right_delim; /* 0 if '.' */
      BtMathNode *body;
    } delim_group;

    struct {
      gunichar codepoint;   /* e.g. 0x2211 for \sum, 0x222B for \int */
      gboolean movable_limits;
      BtLimitsMode limits_mode;
    } large_op;

    struct {
      BtMathNode ***cells;  /* rows * cols array of cell nodes */
      guint rows;
      guint cols;
      gunichar left_delim;  /* e.g. '(' for pmatrix, '[' for bmatrix, '{' for cases */
      gunichar right_delim; /* e.g. ')' for pmatrix, 0 for cases */
      gboolean is_cases;
      gboolean is_aligned;
    } matrix;

    struct {
      int mu; /* 18 mu = 1 em */
    } space;

    struct {
      int math_style; /* 0 = D, 1 = T, 2 = S, 3 = SS */
    } style_switch;

    struct {
      BtMathNode *base;
      BtMathNode *over;
      BtMathNode *under;
    } overunder;

    struct {
      const char *label;
    } error;
  } u;
};

typedef struct {
  gsize offset;
  gsize length;
  char *message;
} BtMathError;

typedef struct BtMathArena BtMathArena;

typedef struct {
  BtMathNode *root;
  BtMathMode mode;
  guint node_count;
  guint max_depth_reached;
  gboolean had_fatal_limit;
  GArray *errors; /* Array of BtMathError */
  BtMathArena *arena;
} BtMathParseResult;

/* Parses a raw LaTeX math string (without surrounding $ or $$ delimiters) into an
 * arena-allocated AST. Never crashes or aborts on malformed input. */
BtMathParseResult *bt_math_parse(const char *src, gsize len, BtMathMode mode);

void bt_math_parse_result_free(BtMathParseResult *res);

G_END_DECLS

#endif /* BT_MATHPARSE_H */
