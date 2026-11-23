/* mathparse.c — Recursive-descent LaTeX math subset parser to arena AST (M2). */
#include "mathparse.h"

#include <string.h>

/* ---- Arena allocator ---- */

#define ARENA_BLOCK_SIZE 8192u

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  gsize used;
  gsize cap;
  guint8 data[];
} ArenaBlock;

struct BtMathArena {
  ArenaBlock *head;
};

static BtMathArena *arena_new(void)
{
  return g_new0(BtMathArena, 1);
}

static void *arena_alloc0(BtMathArena *a, gsize size)
{
  gsize aligned = (size + 7u) & ~(gsize)7u;
  if (a->head == NULL || a->head->used + aligned > a->head->cap) {
    gsize cap = MAX(ARENA_BLOCK_SIZE, aligned + 64u);
    ArenaBlock *b = g_malloc0(sizeof(ArenaBlock) + cap);
    b->cap = cap;
    b->used = 0;
    b->next = a->head;
    a->head = b;
  }
  void *ptr = a->head->data + a->head->used;
  a->head->used += aligned;
  return ptr;
}

static char *arena_strndup(BtMathArena *a, const char *s, gsize n)
{
  char *out = arena_alloc0(a, n + 1);
  if (n > 0 && s != NULL)
    memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static void arena_free(BtMathArena *a)
{
  if (a == NULL)
    return;
  ArenaBlock *b = a->head;
  while (b != NULL) {
    ArenaBlock *next = b->next;
    g_free(b);
    b = next;
  }
  g_free(a);
}

/* ---- Parser state ---- */

typedef struct {
  const char *src;
  gsize len;
  gsize pos;
  guint depth;
  BtMathFontStyle font_style;
  BtMathParseResult *res;
} Parser;

static void add_error(Parser *p, gsize off, gsize len, const char *fmt, ...) G_GNUC_PRINTF(4, 5);

static void add_error(Parser *p, gsize off, gsize len, const char *fmt, ...)
{
  if (p->res->errors->len >= BT_MATH_MAX_ERRORS)
    return;
  va_list ap;
  va_start(ap, fmt);
  BtMathError err;
  err.offset = off;
  err.length = len;
  err.message = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_array_append_val(p->res->errors, err);
}

static void clear_math_error(gpointer data)
{
  BtMathError *e = data;
  g_free(e->message);
}

static BtMathNode *alloc_node(Parser *p, BtMathNodeKind kind, BtAtomClass cls, gsize off, gsize len)
{
  if (p->res->node_count >= BT_MATH_MAX_NODES) {
    if (!p->res->had_fatal_limit) {
      p->res->had_fatal_limit = TRUE;
      add_error(p, off, len, "Formula exceeds maximum node limit (%u)", BT_MATH_MAX_NODES);
    }
    return NULL;
  }
  p->res->node_count++;
  BtMathNode *n = arena_alloc0(p->res->arena, sizeof(BtMathNode));
  n->kind = kind;
  n->atom_class = cls;
  n->src_offset = off;
  n->src_len = len;
  return n;
}

static BtMathNode *make_error_node(Parser *p, gsize off, gsize len, const char *label)
{
  BtMathNode *n = alloc_node(p, BT_NODE_ERROR, BT_ATOM_ORD, off, len);
  if (n != NULL)
    n->u.error.label = arena_strndup(p->res->arena, label ? label : "?", strlen(label ? label : "?"));
  return n;
}

static void skip_ws_and_comments(Parser *p)
{
  while (p->pos < p->len) {
    char c = p->src[p->pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      p->pos++;
    } else if (c == '%') {
      while (p->pos < p->len && p->src[p->pos] != '\n')
        p->pos++;
    } else {
      break;
    }
  }
}

static gboolean enter_depth(Parser *p, gsize off)
{
  if (p->depth >= BT_MATH_MAX_DEPTH) {
    if (!p->res->had_fatal_limit) {
      p->res->had_fatal_limit = TRUE;
      add_error(p, off, 1, "Formula exceeds maximum nesting depth (%u)", BT_MATH_MAX_DEPTH);
    }
    return FALSE;
  }
  p->depth++;
  if (p->depth > p->res->max_depth_reached)
    p->res->max_depth_reached = p->depth;
  return TRUE;
}

static void leave_depth(Parser *p)
{
  if (p->depth > 0)
    p->depth--;
}

/* Forward declarations */
static BtMathNode *parse_row(Parser *p, char stop_char, gboolean stop_on_right,
                             gboolean stop_on_cell, const char *stop_env);
static BtMathNode *parse_primary_atom(Parser *p);
static BtMathNode *parse_required_arg(Parser *p);

static BtMathNode *wrap_in_row(Parser *p, GPtrArray *items, gsize start_off, gsize end_off)
{
  if (items->len == 1)
    return g_ptr_array_index(items, 0);
  BtMathNode *row = alloc_node(p, BT_NODE_ROW, BT_ATOM_ORD, start_off,
                               (end_off >= start_off) ? (end_off - start_off) : 0);
  if (row == NULL)
    return NULL;
  row->u.row.count = items->len;
  if (items->len > 0) {
    row->u.row.items = arena_alloc0(p->res->arena, sizeof(BtMathNode *) * items->len);
    for (guint i = 0; i < items->len; i++)
      row->u.row.items[i] = g_ptr_array_index(items, i);
  }
  return row;
}

/* Reads {ident} used by \begin{env}, \end{env}, \operatorname{name}. */
static gboolean read_braced_ident(Parser *p, char *buf, gsize buf_cap, gsize *out_off, gsize *out_len)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len || p->src[p->pos] != '{')
    return FALSE;
  gsize open_off = p->pos;
  p->pos++; /* consume '{' */
  gsize s = p->pos;
  while (p->pos < p->len && p->src[p->pos] != '}' && p->src[p->pos] != '\n')
    p->pos++;
  gsize elen = p->pos - s;
  if (p->pos < p->len && p->src[p->pos] == '}') {
    p->pos++;
  } else {
    add_error(p, open_off, p->pos - open_off, "Unclosed '{' in identifier");
  }
  while (elen > 0 && (p->src[s] == ' ' || p->src[s] == '\t')) { s++; elen--; }
  while (elen > 0 && (p->src[s + elen - 1] == ' ' || p->src[s + elen - 1] == '\t')) { elen--; }
  gsize copy_len = MIN(elen, buf_cap - 1);
  memcpy(buf, p->src + s, copy_len);
  buf[copy_len] = '\0';
  if (out_off) *out_off = open_off;
  if (out_len) *out_len = p->pos - open_off;
  return TRUE;
}

/* Reads a delimiter token after \left, \right, \middle, \big, etc. */
static gunichar read_delimiter_token(Parser *p)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len) {
    add_error(p, p->pos, 0, "Expected delimiter at end of formula");
    return 0;
  }
  gsize s = p->pos;
  if (p->src[p->pos] == '\\') {
    p->pos++;
    if (p->pos < p->len) {
      if (g_ascii_isalpha(p->src[p->pos])) {
        while (p->pos < p->len && g_ascii_isalpha(p->src[p->pos]))
          p->pos++;
      } else {
        p->pos++;
      }
    }
    return bt_math_resolve_delimiter(p->src + s, p->pos - s);
  }
  p->pos++;
  return bt_math_resolve_delimiter(p->src + s, 1);
}

/* Reads raw text inside { ... } for \text{...} with nested brace balance. */
static BtMathNode *parse_text_box(Parser *p, BtMathFontStyle style, gsize cmd_off)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len || p->src[p->pos] != '{') {
    add_error(p, cmd_off, p->pos - cmd_off, "Expected '{' after \\text");
    return make_error_node(p, cmd_off, p->pos - cmd_off, "\\text?");
  }
  p->pos++; /* consume '{' */
  gsize s = p->pos;
  int depth = 1;
  GString *str = g_string_new(NULL);
  while (p->pos < p->len && depth > 0) {
    char c = p->src[p->pos];
    if (c == '\\' && p->pos + 1 < p->len) {
      char next = p->src[p->pos + 1];
      if (next == '{' || next == '}' || next == '%' || next == '$' || next == '&' || next == '#' || next == '_') {
        g_string_append_c(str, next);
        p->pos += 2;
        continue;
      } else if (next == ' ') {
        g_string_append_c(str, ' ');
        p->pos += 2;
        continue;
      }
    }
    if (c == '{') {
      depth++;
      g_string_append_c(str, c);
    } else if (c == '}') {
      depth--;
      if (depth > 0)
        g_string_append_c(str, c);
    } else if (c == '\r' || c == '\n') {
      g_string_append_c(str, ' ');
    } else {
      g_string_append_c(str, c);
    }
    p->pos++;
  }
  if (depth != 0)
    add_error(p, s - 1, p->pos - (s - 1), "Unclosed '{' in \\text");

  BtMathNode *n = alloc_node(p, BT_NODE_TEXT, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
  if (n != NULL) {
    n->u.text.text = arena_strndup(p->res->arena, str->str, str->len);
    n->u.text.font_style = style;
    n->u.text.is_operator = FALSE;
  }
  g_string_free(str, TRUE);
  return n;
}

static gboolean peek_is_end_env(Parser *p, const char *env_name)
{
  if (env_name == NULL)
    return FALSE;
  gsize saved = p->pos;
  skip_ws_and_comments(p);
  gboolean match = FALSE;
  if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "\\end", 4) == 0 &&
      (p->pos + 4 == p->len || !g_ascii_isalpha(p->src[p->pos + 4]))) {
    p->pos += 4;
    char buf[64];
    if (read_braced_ident(p, buf, sizeof(buf), NULL, NULL)) {
      if (strcmp(buf, env_name) == 0)
        match = TRUE;
    }
  }
  p->pos = saved;
  return match;
}

static BtMathNode *parse_environment(Parser *p, gsize cmd_off)
{
  char env[64];
  gsize id_off = 0, id_len = 0;
  if (!read_braced_ident(p, env, sizeof(env), &id_off, &id_len)) {
    add_error(p, cmd_off, p->pos - cmd_off, "Expected '{env}' after \\begin");
    return make_error_node(p, cmd_off, p->pos - cmd_off, "\\begin?");
  }

  /* Equation / gather wrappers just parse as a row or single-column aligned matrix */
  gboolean is_matrix = FALSE;
  gboolean is_cases = FALSE;
  gboolean is_aligned = FALSE;
  gunichar ldelim = 0, rdelim = 0;

  if (strcmp(env, "matrix") == 0) {
    is_matrix = TRUE;
  } else if (strcmp(env, "pmatrix") == 0) {
    is_matrix = TRUE; ldelim = '('; rdelim = ')';
  } else if (strcmp(env, "bmatrix") == 0) {
    is_matrix = TRUE; ldelim = '['; rdelim = ']';
  } else if (strcmp(env, "Bmatrix") == 0) {
    is_matrix = TRUE; ldelim = '{'; rdelim = '}';
  } else if (strcmp(env, "vmatrix") == 0) {
    is_matrix = TRUE; ldelim = '|'; rdelim = '|';
  } else if (strcmp(env, "Vmatrix") == 0) {
    is_matrix = TRUE; ldelim = 0x2016; rdelim = 0x2016;
  } else if (strcmp(env, "cases") == 0 || strcmp(env, "dcases") == 0) {
    is_matrix = TRUE; is_cases = TRUE; ldelim = '{'; rdelim = 0;
  } else if (strcmp(env, "aligned") == 0 || strcmp(env, "align") == 0 ||
             strcmp(env, "align*") == 0 || strcmp(env, "split") == 0 ||
             strcmp(env, "gathered") == 0 || strcmp(env, "gather") == 0 ||
             strcmp(env, "gather*") == 0 || strcmp(env, "eqnarray") == 0 ||
             strcmp(env, "eqnarray*") == 0) {
    is_matrix = TRUE; is_aligned = TRUE;
  } else if (strcmp(env, "array") == 0) {
    is_matrix = TRUE;
    /* Optional/required column spec {ccc} */
    skip_ws_and_comments(p);
    if (p->pos < p->len && p->src[p->pos] == '{') {
      char colspec[64];
      read_braced_ident(p, colspec, sizeof(colspec), NULL, NULL);
    }
  } else if (strcmp(env, "equation") == 0 || strcmp(env, "equation*") == 0 ||
             strcmp(env, "math") == 0 || strcmp(env, "displaymath") == 0) {
    is_matrix = TRUE; is_aligned = TRUE;
  } else {
    add_error(p, id_off, id_len, "Unsupported environment '%s'", env);
    return make_error_node(p, cmd_off, p->pos - cmd_off, env);
  }

  (void)is_matrix;
  if (!enter_depth(p, cmd_off))
    return make_error_node(p, cmd_off, p->pos - cmd_off, "depth!");

  GPtrArray *rows = g_ptr_array_new();
  guint max_cols = 0;
  gboolean closed = FALSE;

  while (p->pos < p->len && !p->res->had_fatal_limit) {
    if (peek_is_end_env(p, env)) {
      /* Consume \end{env} */
      skip_ws_and_comments(p);
      p->pos += 4;
      char end_buf[64];
      read_braced_ident(p, end_buf, sizeof(end_buf), NULL, NULL);
      closed = TRUE;
      break;
    }

    GPtrArray *row_cells = g_ptr_array_new();
    while (p->pos < p->len && !p->res->had_fatal_limit) {
      BtMathNode *cell = parse_row(p, '\0', FALSE, TRUE, env);
      g_ptr_array_add(row_cells, cell);

      skip_ws_and_comments(p);
      if (p->pos < p->len && p->src[p->pos] == '&') {
        p->pos++; /* next column */
        if (row_cells->len >= BT_MATH_MAX_MATRIX_DIM) {
          add_error(p, p->pos - 1, 1, "Matrix exceeds maximum column count (%u)", BT_MATH_MAX_MATRIX_DIM);
          p->res->had_fatal_limit = TRUE;
          break;
        }
        continue;
      }
      if (p->pos + 1 < p->len && p->src[p->pos] == '\\' &&
          (p->src[p->pos + 1] == '\\' ||
           (p->pos + 3 <= p->len && memcmp(p->src + p->pos, "\\cr", 3) == 0 &&
            (p->pos + 3 == p->len || !g_ascii_isalpha(p->src[p->pos + 3]))))) {
        if (p->src[p->pos + 1] == '\\')
          p->pos += 2;
        else
          p->pos += 3;
        /* Optional *[len] or [len] after \\ */
        skip_ws_and_comments(p);
        if (p->pos < p->len && p->src[p->pos] == '*') {
          p->pos++;
          skip_ws_and_comments(p);
        }
        if (p->pos < p->len && p->src[p->pos] == '[') {
          while (p->pos < p->len && p->src[p->pos] != ']')
            p->pos++;
          if (p->pos < p->len)
            p->pos++;
        }
        break; /* end of row */
      }
      break;
    }

    /* Skip trailing empty row just before \end{env} */
    gboolean is_empty_trailing = FALSE;
    if (row_cells->len == 1 && peek_is_end_env(p, env)) {
      BtMathNode *only = g_ptr_array_index(row_cells, 0);
      if (only == NULL || (only->kind == BT_NODE_ROW && only->u.row.count == 0))
        is_empty_trailing = TRUE;
    }

    if (!is_empty_trailing) {
      if (row_cells->len > max_cols)
        max_cols = row_cells->len;
      g_ptr_array_add(rows, row_cells);
      if (rows->len > BT_MATH_MAX_MATRIX_DIM) {
        add_error(p, p->pos, 1, "Matrix exceeds maximum row count (%u)", BT_MATH_MAX_MATRIX_DIM);
        p->res->had_fatal_limit = TRUE;
        break;
      }
    } else {
      g_ptr_array_free(row_cells, TRUE);
    }

    if (p->pos >= p->len)
      break;
  }

  leave_depth(p);

  if (!closed && !p->res->had_fatal_limit)
    add_error(p, cmd_off, p->pos - cmd_off, "Unclosed \\begin{%s}", env);

  if (rows->len == 0) {
    GPtrArray *empty_row = g_ptr_array_new();
    g_ptr_array_add(empty_row, alloc_node(p, BT_NODE_ROW, BT_ATOM_ORD, cmd_off, 0));
    g_ptr_array_add(rows, empty_row);
    max_cols = 1;
  }
  if (max_cols == 0)
    max_cols = 1;

  BtMathNode *mat = alloc_node(p, BT_NODE_MATRIX, BT_ATOM_INNER, cmd_off, p->pos - cmd_off);
  if (mat != NULL) {
    mat->u.matrix.rows = rows->len;
    mat->u.matrix.cols = max_cols;
    mat->u.matrix.left_delim = ldelim;
    mat->u.matrix.right_delim = rdelim;
    mat->u.matrix.is_cases = is_cases;
    mat->u.matrix.is_aligned = is_aligned;
    mat->u.matrix.cells = arena_alloc0(p->res->arena, sizeof(BtMathNode **) * rows->len);
    for (guint r = 0; r < rows->len; r++) {
      GPtrArray *rc = g_ptr_array_index(rows, r);
      mat->u.matrix.cells[r] = arena_alloc0(p->res->arena, sizeof(BtMathNode *) * max_cols);
      for (guint c = 0; c < max_cols; c++) {
        if (c < rc->len)
          mat->u.matrix.cells[r][c] = g_ptr_array_index(rc, c);
        else
          mat->u.matrix.cells[r][c] = alloc_node(p, BT_NODE_ROW, BT_ATOM_ORD, cmd_off, 0);
      }
    }
  }

  for (guint r = 0; r < rows->len; r++)
    g_ptr_array_free(g_ptr_array_index(rows, r), TRUE);
  g_ptr_array_free(rows, TRUE);
  return mat;
}

static BtMathNode *parse_required_arg(Parser *p)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len) {
    add_error(p, p->pos, 0, "Missing required argument at end of formula");
    return make_error_node(p, p->pos, 0, "?");
  }
  if (p->src[p->pos] == '{') {
    gsize open_off = p->pos;
    p->pos++; /* consume '{' */
    if (!enter_depth(p, open_off))
      return make_error_node(p, open_off, 1, "depth!");
    BtMathNode *body = parse_row(p, '}', FALSE, FALSE, NULL);
    leave_depth(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
      p->pos++;
    } else {
      add_error(p, open_off, p->pos - open_off, "Unclosed '{'");
    }
    return body;
  }
  return parse_primary_atom(p);
}

static BtMathNode *parse_bracket_opt_arg(Parser *p)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len || p->src[p->pos] != '[')
    return NULL;
  gsize open_off = p->pos;
  p->pos++; /* consume '[' */
  if (!enter_depth(p, open_off))
    return NULL;
  BtMathNode *body = parse_row(p, ']', FALSE, FALSE, NULL);
  leave_depth(p);
  if (p->pos < p->len && p->src[p->pos] == ']') {
    p->pos++;
  } else {
    add_error(p, open_off, p->pos - open_off, "Unclosed '[' in optional argument");
  }
  return body;
}

static BtMathNode *parse_command(Parser *p)
{
  gsize cmd_off = p->pos;
  p->pos++; /* consume '\\' */
  if (p->pos >= p->len) {
    add_error(p, cmd_off, 1, "Trailing backslash at end of formula");
    return make_error_node(p, cmd_off, 1, "\\");
  }

  gsize name_start = p->pos;
  if (g_ascii_isalpha(p->src[p->pos])) {
    while (p->pos < p->len && g_ascii_isalpha(p->src[p->pos]))
      p->pos++;
  } else {
    p->pos++;
  }
  gsize name_len = p->pos - name_start;
  const char *name = p->src + name_start;

  const BtMathCmdEntry *cmd = bt_math_cmd_lookup(name, name_len);
  if (cmd == NULL) {
    g_autofree char *unk = g_strdup_printf("\\%.*s", (int)name_len, name);
    add_error(p, cmd_off, p->pos - cmd_off, "Unknown command '%s'", unk);
    return make_error_node(p, cmd_off, p->pos - cmd_off, unk);
  }

  switch (cmd->kind) {
    case BT_CMD_FORBIDDEN: {
      g_autofree char *lbl = g_strdup_printf("\\%.*s", (int)name_len, name);
      add_error(p, cmd_off, p->pos - cmd_off, "Forbidden TeX primitive '%s'", lbl);
      return make_error_node(p, cmd_off, p->pos - cmd_off, lbl);
    }

    case BT_CMD_SYMBOL: {
      BtMathNode *n = alloc_node(p, BT_NODE_SYMBOL, cmd->atom_class, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.symbol.base_char = cmd->codepoint;
        n->u.symbol.font_style = p->font_style;
        n->u.symbol.codepoint = bt_math_map_styled_char(cmd->codepoint, p->font_style, TRUE);
      }
      return n;
    }

    case BT_CMD_LARGE_OP: {
      BtMathNode *n = alloc_node(p, BT_NODE_LARGE_OP, BT_ATOM_OP, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.large_op.codepoint = cmd->codepoint;
        n->u.large_op.movable_limits = (cmd->aux != 0);
        n->u.large_op.limits_mode = BT_LIMITS_AUTO;
      }
      return n;
    }

    case BT_CMD_NAMED_OP: {
      BtMathNode *n = alloc_node(p, BT_NODE_TEXT, BT_ATOM_OP, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.text.text = arena_strndup(p->res->arena, cmd->op_label, strlen(cmd->op_label));
        n->u.text.font_style = BT_MATH_FONT_ROMAN;
        n->u.text.is_operator = TRUE;
        n->u.text.movable_limits = (cmd->aux != 0);
        n->u.text.limits_mode = BT_LIMITS_AUTO;
      }
      return n;
    }

    case BT_CMD_OPERATORNAME: {
      skip_ws_and_comments(p);
      gboolean starred = FALSE;
      if (p->pos < p->len && p->src[p->pos] == '*') {
        starred = TRUE;
        p->pos++;
      }
      char opbuf[64];
      if (!read_braced_ident(p, opbuf, sizeof(opbuf), NULL, NULL)) {
        add_error(p, cmd_off, p->pos - cmd_off, "Expected '{name}' after \\operatorname");
        return make_error_node(p, cmd_off, p->pos - cmd_off, "\\operatorname?");
      }
      BtMathNode *n = alloc_node(p, BT_NODE_TEXT, BT_ATOM_OP, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.text.text = arena_strndup(p->res->arena, opbuf, strlen(opbuf));
        n->u.text.font_style = BT_MATH_FONT_ROMAN;
        n->u.text.is_operator = TRUE;
        n->u.text.movable_limits = starred;
        n->u.text.limits_mode = BT_LIMITS_AUTO;
      }
      return n;
    }

    case BT_CMD_FRAC: {
      BtMathNode *num = parse_required_arg(p);
      BtMathNode *den = parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_FRAC, BT_ATOM_INNER, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.frac.num = num;
        n->u.frac.den = den;
        n->u.frac.has_bar = TRUE;
        n->u.frac.style_override = cmd->aux;
        n->u.frac.left_delim = 0;
        n->u.frac.right_delim = 0;
      }
      return n;
    }

    case BT_CMD_BINOM: {
      BtMathNode *num = parse_required_arg(p);
      BtMathNode *den = parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_FRAC, BT_ATOM_INNER, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.frac.num = num;
        n->u.frac.den = den;
        n->u.frac.has_bar = FALSE;
        n->u.frac.style_override = cmd->aux;
        n->u.frac.left_delim = '(';
        n->u.frac.right_delim = ')';
      }
      return n;
    }

    case BT_CMD_SQRT: {
      BtMathNode *idx = parse_bracket_opt_arg(p);
      BtMathNode *rad = parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_SQRT, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.sqrt_node.radicand = rad;
        n->u.sqrt_node.index = idx;
      }
      return n;
    }

    case BT_CMD_ACCENT: {
      BtMathNode *base = parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_ACCENT, cmd->atom_class, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.accent.base = base;
        n->u.accent.accent_kind = (BtAccentKind)cmd->aux;
        n->u.accent.accent_cp = cmd->codepoint;
      }
      return n;
    }

    case BT_CMD_FONT: {
      BtMathFontStyle prev = p->font_style;
      p->font_style = (BtMathFontStyle)cmd->aux;
      BtMathNode *arg = parse_required_arg(p);
      p->font_style = prev;
      return arg;
    }

    case BT_CMD_TEXT:
      return parse_text_box(p, (BtMathFontStyle)cmd->aux, cmd_off);

    case BT_CMD_SPACE: {
      BtMathNode *n = alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
      if (n != NULL)
        n->u.space.mu = cmd->aux;
      return n;
    }

    case BT_CMD_LEFT: {
      gunichar ldelim = read_delimiter_token(p);
      if (!enter_depth(p, cmd_off))
        return make_error_node(p, cmd_off, p->pos - cmd_off, "depth!");
      BtMathNode *body = parse_row(p, '\0', TRUE, FALSE, NULL);
      leave_depth(p);
      gunichar rdelim = 0;
      skip_ws_and_comments(p);
      if (p->pos + 6 <= p->len && memcmp(p->src + p->pos, "\\right", 6) == 0 &&
          (p->pos + 6 == p->len || !g_ascii_isalpha(p->src[p->pos + 6]))) {
        p->pos += 6;
        rdelim = read_delimiter_token(p);
      } else {
        add_error(p, cmd_off, p->pos - cmd_off, "Missing matching \\right for \\left");
      }
      BtMathNode *n = alloc_node(p, BT_NODE_DELIM_GROUP, BT_ATOM_INNER, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.delim_group.left_delim = ldelim;
        n->u.delim_group.right_delim = rdelim;
        n->u.delim_group.body = body;
      }
      return n;
    }

    case BT_CMD_RIGHT: {
      add_error(p, cmd_off, p->pos - cmd_off, "Unmatched \\right");
      (void)read_delimiter_token(p);
      return make_error_node(p, cmd_off, p->pos - cmd_off, "\\right?");
    }

    case BT_CMD_MIDDLE: {
      gunichar mdelim = read_delimiter_token(p);
      BtMathNode *n = alloc_node(p, BT_NODE_SYMBOL, BT_ATOM_REL, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.symbol.codepoint = mdelim ? mdelim : '|';
        n->u.symbol.base_char = n->u.symbol.codepoint;
        n->u.symbol.big_step = 2;
      }
      return n;
    }

    case BT_CMD_BIG_DELIM: {
      gunichar d = read_delimiter_token(p);
      BtMathNode *n = alloc_node(p, BT_NODE_SYMBOL, cmd->atom_class, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.symbol.codepoint = d ? d : '|';
        n->u.symbol.base_char = n->u.symbol.codepoint;
        n->u.symbol.big_step = cmd->aux;
      }
      return n;
    }

    case BT_CMD_STYLE_SWITCH: {
      BtMathNode *n = alloc_node(p, BT_NODE_STYLE_SWITCH, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
      if (n != NULL)
        n->u.style_switch.math_style = cmd->aux;
      return n;
    }

    case BT_CMD_BEGIN:
      return parse_environment(p, cmd_off);

    case BT_CMD_END: {
      char env[64];
      read_braced_ident(p, env, sizeof(env), NULL, NULL);
      add_error(p, cmd_off, p->pos - cmd_off, "Unmatched \\end{%s}", env);
      return make_error_node(p, cmd_off, p->pos - cmd_off, "\\end?");
    }

    case BT_CMD_OVERSET: {
      BtMathNode *top = parse_required_arg(p);
      BtMathNode *base = parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_OVERUNDER, BT_ATOM_REL, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.overunder.base = base;
        n->u.overunder.over = top;
        n->u.overunder.under = NULL;
      }
      return n;
    }

    case BT_CMD_UNDERSET: {
      BtMathNode *bot = parse_required_arg(p);
      BtMathNode *base = parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_OVERUNDER, BT_ATOM_REL, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.overunder.base = base;
        n->u.overunder.over = NULL;
        n->u.overunder.under = bot;
      }
      return n;
    }

    case BT_CMD_PHANTOM: {
      /* Parse argument and return thin space so layout succeeds cleanly */
      (void)parse_required_arg(p);
      BtMathNode *n = alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
      if (n != NULL)
        n->u.space.mu = 0;
      return n;
    }

    case BT_CMD_COLOR_NOOP: {
      if (cmd->aux == 2) {
        /* \color{name} takes 1 braced color name */
        char col[64];
        read_braced_ident(p, col, sizeof(col), NULL, NULL);
        return alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
      } else if (cmd->aux == 3) {
        /* \textcolor{name}{body} */
        char col[64];
        read_braced_ident(p, col, sizeof(col), NULL, NULL);
        return parse_required_arg(p);
      }
      /* \boxed{body} */
      return parse_required_arg(p);
    }

    case BT_CMD_MOD: {
      if (cmd->aux == 1) {
        /* \pmod{n} -> (mod n) */
        BtMathNode *arg = parse_required_arg(p);
        GPtrArray *items = g_ptr_array_new();
        BtMathNode *sp = alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, cmd_off, 0);
        if (sp) sp->u.space.mu = 6;
        BtMathNode *lp = alloc_node(p, BT_NODE_SYMBOL, BT_ATOM_OPEN, cmd_off, 0);
        if (lp) { lp->u.symbol.codepoint = '('; lp->u.symbol.base_char = '('; }
        BtMathNode *op = alloc_node(p, BT_NODE_TEXT, BT_ATOM_OP, cmd_off, 0);
        if (op) { op->u.text.text = "mod"; op->u.text.font_style = BT_MATH_FONT_ROMAN; op->u.text.is_operator = TRUE; }
        BtMathNode *sp2 = alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, cmd_off, 0);
        if (sp2) sp2->u.space.mu = 4;
        BtMathNode *rp = alloc_node(p, BT_NODE_SYMBOL, BT_ATOM_CLOSE, cmd_off, 0);
        if (rp) { rp->u.symbol.codepoint = ')'; rp->u.symbol.base_char = ')'; }
        if (sp) g_ptr_array_add(items, sp);
        if (lp) g_ptr_array_add(items, lp);
        if (op) g_ptr_array_add(items, op);
        if (sp2) g_ptr_array_add(items, sp2);
        if (arg) g_ptr_array_add(items, arg);
        if (rp) g_ptr_array_add(items, rp);
        BtMathNode *row = wrap_in_row(p, items, cmd_off, p->pos);
        g_ptr_array_free(items, TRUE);
        return row;
      }
      BtMathNode *n = alloc_node(p, BT_NODE_TEXT, BT_ATOM_BIN, cmd_off, p->pos - cmd_off);
      if (n != NULL) {
        n->u.text.text = "mod";
        n->u.text.font_style = BT_MATH_FONT_ROMAN;
        n->u.text.is_operator = TRUE;
      }
      return n;
    }

    case BT_CMD_NOT: {
      BtMathNode *next = parse_primary_atom(p);
      if (next != NULL && next->kind == BT_NODE_SYMBOL) {
        switch (next->u.symbol.codepoint) {
          case '=':    next->u.symbol.codepoint = 0x2260; break;
          case 0x2208: next->u.symbol.codepoint = 0x2209; break;
          case 0x2282: next->u.symbol.codepoint = 0x2284; break;
          case 0x2286: next->u.symbol.codepoint = 0x2288; break;
          case 0x2261: next->u.symbol.codepoint = 0x2262; break;
          case '<':    next->u.symbol.codepoint = 0x226E; break;
          case '>':    next->u.symbol.codepoint = 0x226F; break;
          case 0x2264: next->u.symbol.codepoint = 0x2270; break;
          case 0x2265: next->u.symbol.codepoint = 0x2271; break;
          default: break;
        }
      }
      return next;
    }

    case BT_CMD_LIMITS_MOD:
      /* Handled as postfix in parse_atom_with_scripts; if standalone, ignore */
      return alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, cmd_off, p->pos - cmd_off);
  }
  return NULL;
}

static BtMathNode *parse_primary_atom(Parser *p)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len || p->res->had_fatal_limit)
    return NULL;

  char c = p->src[p->pos];
  if (c == '{') {
    gsize open_off = p->pos;
    p->pos++;
    if (!enter_depth(p, open_off))
      return make_error_node(p, open_off, 1, "depth!");
    BtMathNode *body = parse_row(p, '}', FALSE, FALSE, NULL);
    leave_depth(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
      p->pos++;
    } else {
      add_error(p, open_off, p->pos - open_off, "Unclosed '{'");
    }
    return body;
  }

  if (c == '}') {
    add_error(p, p->pos, 1, "Unmatched '}'");
    p->pos++;
    return make_error_node(p, p->pos - 1, 1, "}");
  }

  if (c == '\\')
    return parse_command(p);

  if (c == '~') {
    p->pos++;
    BtMathNode *n = alloc_node(p, BT_NODE_SPACE, BT_ATOM_ORD, p->pos - 1, 1);
    if (n) n->u.space.mu = 6;
    return n;
  }

  /* Read UTF-8 character */
  gsize ch_off = p->pos;
  gunichar uc = g_utf8_get_char_validated(p->src + p->pos, (gssize)(p->len - p->pos));
  if (uc == (gunichar)-1 || uc == (gunichar)-2) {
    p->pos++;
    uc = 0xFFFDu;
  } else {
    const char *next = g_utf8_next_char(p->src + p->pos);
    p->pos = (gsize)(next - p->src);
  }

  /* ASCII hyphen '-' in math mode is Unicode minus sign U+2212 */
  gunichar base_uc = uc;
  if (uc == '-')
    uc = 0x2212u;

  BtAtomClass cls = bt_math_char_atom_class(uc);
  gunichar styled = bt_math_map_styled_char(uc, p->font_style, TRUE);
  BtMathNode *n = alloc_node(p, BT_NODE_SYMBOL, cls, ch_off, p->pos - ch_off);
  if (n != NULL) {
    n->u.symbol.codepoint = styled;
    n->u.symbol.base_char = base_uc;
    n->u.symbol.font_style = p->font_style;
  }
  return n;
}

static BtMathNode *parse_atom_with_scripts(Parser *p)
{
  skip_ws_and_comments(p);
  if (p->pos >= p->len || p->res->had_fatal_limit)
    return NULL;

  BtMathNode *base = NULL;
  gsize start_off = p->pos;

  if (p->src[p->pos] == '^' || p->src[p->pos] == '_') {
    /* Orphan script: synthesize empty base node */
    base = alloc_node(p, BT_NODE_ROW, BT_ATOM_ORD, start_off, 0);
  } else {
    base = parse_primary_atom(p);
    if (base == NULL)
      return NULL;
  }

  BtMathNode *sub = NULL;
  BtMathNode *sup = NULL;

  for (;;) {
    skip_ws_and_comments(p);
    if (p->pos >= p->len || p->res->had_fatal_limit)
      break;

    /* Check for \limits or \nolimits after an operator */
    if (p->src[p->pos] == '\\') {
      if (p->pos + 7 <= p->len && memcmp(p->src + p->pos, "\\limits", 7) == 0 &&
          (p->pos + 7 == p->len || !g_ascii_isalpha(p->src[p->pos + 7]))) {
        p->pos += 7;
        if (base->kind == BT_NODE_LARGE_OP)
          base->u.large_op.limits_mode = BT_LIMITS_ALWAYS;
        else if (base->kind == BT_NODE_TEXT)
          base->u.text.limits_mode = BT_LIMITS_ALWAYS;
        continue;
      }
      if (p->pos + 9 <= p->len && memcmp(p->src + p->pos, "\\nolimits", 9) == 0 &&
          (p->pos + 9 == p->len || !g_ascii_isalpha(p->src[p->pos + 9]))) {
        p->pos += 9;
        if (base->kind == BT_NODE_LARGE_OP)
          base->u.large_op.limits_mode = BT_LIMITS_NEVER;
        else if (base->kind == BT_NODE_TEXT)
          base->u.text.limits_mode = BT_LIMITS_NEVER;
        continue;
      }
    }

    /* Prime marks: f', f'', f''' */
    if (p->src[p->pos] == '\'') {
      gsize p_off = p->pos;
      guint n_primes = 0;
      while (p->pos < p->len && p->src[p->pos] == '\'') {
        n_primes++;
        p->pos++;
      }
      gunichar prime_cp = (n_primes == 1) ? 0x2032u : (n_primes == 2 ? 0x2033u : 0x2034u);
      BtMathNode *pnode = alloc_node(p, BT_NODE_SYMBOL, BT_ATOM_ORD, p_off, p->pos - p_off);
      if (pnode != NULL) {
        pnode->u.symbol.codepoint = prime_cp;
        pnode->u.symbol.base_char = '\'';
      }
      if (sup == NULL) {
        sup = pnode;
      } else {
        GPtrArray *arr = g_ptr_array_new();
        g_ptr_array_add(arr, sup);
        if (pnode) g_ptr_array_add(arr, pnode);
        sup = wrap_in_row(p, arr, p_off, p->pos);
        g_ptr_array_free(arr, TRUE);
      }
      continue;
    }

    if (p->src[p->pos] == '^') {
      gsize op_off = p->pos;
      p->pos++;
      BtMathNode *arg = parse_required_arg(p);
      if (sup != NULL) {
        add_error(p, op_off, 1, "Double superscript");
        GPtrArray *arr = g_ptr_array_new();
        g_ptr_array_add(arr, sup);
        if (arg) g_ptr_array_add(arr, arg);
        sup = wrap_in_row(p, arr, op_off, p->pos);
        g_ptr_array_free(arr, TRUE);
      } else {
        sup = arg;
      }
      continue;
    }

    if (p->src[p->pos] == '_') {
      gsize op_off = p->pos;
      p->pos++;
      BtMathNode *arg = parse_required_arg(p);
      if (sub != NULL) {
        add_error(p, op_off, 1, "Double subscript");
        GPtrArray *arr = g_ptr_array_new();
        g_ptr_array_add(arr, sub);
        if (arg) g_ptr_array_add(arr, arg);
        sub = wrap_in_row(p, arr, op_off, p->pos);
        g_ptr_array_free(arr, TRUE);
      } else {
        sub = arg;
      }
      continue;
    }

    break;
  }

  if (sub == NULL && sup == NULL)
    return base;

  BtMathNode *sc = alloc_node(p, BT_NODE_SCRIPTS, base->atom_class, start_off, p->pos - start_off);
  if (sc != NULL) {
    sc->u.scripts.base = base;
    sc->u.scripts.sub = sub;
    sc->u.scripts.sup = sup;
  }
  return sc;
}

static BtMathNode *parse_row(Parser *p, char stop_char, gboolean stop_on_right,
                             gboolean stop_on_cell, const char *stop_env)
{
  gsize start_off = p->pos;
  GPtrArray *items = g_ptr_array_new();

  while (p->pos < p->len && !p->res->had_fatal_limit) {
    skip_ws_and_comments(p);
    if (p->pos >= p->len)
      break;

    char c = p->src[p->pos];
    if (stop_char != '\0' && c == stop_char)
      break;
    if (c == '}' && stop_char != '}') {
      /* Unexpected closing brace: consume and report error if at top level, else stop */
      if (p->depth > 0)
        break;
    }

    if (stop_on_cell) {
      if (c == '&')
        break;
      if (c == '\\' && p->pos + 1 < p->len &&
          (p->src[p->pos + 1] == '\\' ||
           (p->pos + 3 <= p->len && memcmp(p->src + p->pos, "\\cr", 3) == 0 &&
            (p->pos + 3 == p->len || !g_ascii_isalpha(p->src[p->pos + 3]))))) {
        break;
      }
    }

    if (stop_on_right && c == '\\' && p->pos + 6 <= p->len &&
        memcmp(p->src + p->pos, "\\right", 6) == 0 &&
        (p->pos + 6 == p->len || !g_ascii_isalpha(p->src[p->pos + 6]))) {
      break;
    }

    if (stop_env != NULL && peek_is_end_env(p, stop_env))
      break;

    gsize before = p->pos;
    BtMathNode *atom = parse_atom_with_scripts(p);
    if (atom != NULL)
      g_ptr_array_add(items, atom);
    if (p->pos == before)
      p->pos++; /* guarantee forward progress */
  }

  BtMathNode *row = alloc_node(p, BT_NODE_ROW, BT_ATOM_ORD, start_off,
                               (p->pos >= start_off) ? (p->pos - start_off) : 0);
  if (row != NULL) {
    row->u.row.count = items->len;
    if (items->len > 0) {
      row->u.row.items = arena_alloc0(p->res->arena, sizeof(BtMathNode *) * items->len);
      for (guint i = 0; i < items->len; i++)
        row->u.row.items[i] = g_ptr_array_index(items, i);
    }
  }
  g_ptr_array_free(items, TRUE);
  return row;
}

BtMathParseResult *bt_math_parse(const char *src, gsize len, BtMathMode mode)
{
  BtMathParseResult *res = g_new0(BtMathParseResult, 1);
  res->mode = mode;
  res->arena = arena_new();
  res->errors = g_array_new(FALSE, TRUE, sizeof(BtMathError));
  g_array_set_clear_func(res->errors, clear_math_error);

  if (src == NULL)
    src = "";

  gsize max_bytes = (mode == BT_MATH_DISPLAY) ? BT_MATH_MAX_DISPLAY_BYTES : BT_MATH_MAX_INLINE_BYTES;
  Parser p = {
    .src = src,
    .len = len,
    .pos = 0,
    .depth = 0,
    .font_style = BT_MATH_FONT_NORMAL,
    .res = res,
  };

  if (len > max_bytes) {
    res->had_fatal_limit = TRUE;
    add_error(&p, 0, len, "Formula length (%zu bytes) exceeds maximum (%zu bytes)", len, max_bytes);
    res->root = make_error_node(&p, 0, len, "Formula too long");
    return res;
  }

  res->root = parse_row(&p, '\0', FALSE, FALSE, NULL);
  return res;
}

void bt_math_parse_result_free(BtMathParseResult *res)
{
  if (res == NULL)
    return;
  if (res->errors != NULL)
    g_array_unref(res->errors);
  arena_free(res->arena);
  g_free(res);
}
