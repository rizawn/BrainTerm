/* theme.h — BrainTerm design tokens ("Dusk" theme). Single source of truth for
 * Cairo / terminal / editor-tag colors. MUST stay in sync with the @define-color
 * tokens in data/theme.css (verified by tests/unit/test_theme.c). */
#ifndef BT_THEME_H
#define BT_THEME_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BT_SCHEME_DARK = 0,
  BT_SCHEME_LIGHT = 1,
  BT_SCHEME_SYSTEM = 2
} BtUiScheme;

/* ----------------------------------------------------------- color tokens -- */
/* 0x00RRGGBB */
#define BT_COL_BG_BASE        0x0E0F12u
#define BT_COL_SURFACE_1      0x15171Cu
#define BT_COL_SURFACE_2      0x1B1E24u
#define BT_COL_BORDER         0x262A32u
#define BT_COL_BORDER_STRONG  0x343944u
#define BT_COL_TEXT_PRIMARY   0xE6E8ECu
#define BT_COL_TEXT_SECONDARY 0x9AA1ADu
#define BT_COL_TEXT_MUTED     0x666D79u
#define BT_COL_ACCENT         0x8B7CF6u
#define BT_COL_SUCCESS        0x4ADE80u
#define BT_COL_WARNING        0xFBBF24u
#define BT_COL_DANGER         0xF87171u

/* Brain Graph v2 role tokens (mirrored in data/theme.css). */
#define BT_COL_GRAPH_BG          0x0B0C10u
#define BT_COL_GRAPH_VIGNETTE    0x06070Au
#define BT_COL_GRAPH_NODE_GLOW   0x9E91FAu
#define BT_COL_GRAPH_EDGE_DIM    0x6E778Au
#define BT_COL_GRAPH_EDGE_ACTIVE 0xA79BFAu
#define BT_COL_GRAPH_DUST        0x7A8296u

/* Editor clickable-link tokens (mirrored in data/theme.css, L2-3). */
#define BT_COL_LINK              0x8B7CF6u
#define BT_COL_LINK_HOVER        0xA79BFAu
#define BT_COL_LINK_BROKEN       0xD4A12Au
#define BT_COL_LINK_BLOCKED      0xD96B6Bu

/* Alpha tokens. */
#define BT_ALPHA_ACCENT_SOFT  0.14
#define BT_ALPHA_ACCENT_SEL   0.30

/* Geometry tokens (px). */
#define BT_RADIUS_PANEL   8
#define BT_RADIUS_CONTROL 6
#define BT_SPACE_1 4
#define BT_SPACE_2 8
#define BT_SPACE_3 12
#define BT_SPACE_4 16
#define BT_SPACE_5 24

/* Node-type colors for the graph (derived from tokens above). */
#define BT_COL_NODE_NOTE   BT_COL_TEXT_PRIMARY
#define BT_COL_NODE_AI     BT_COL_ACCENT
#define BT_COL_NODE_TAG    BT_COL_WARNING
#define BT_COL_NODE_FOLDER BT_COL_SUCCESS

/* Split a 0x00RRGGBB token into 0..1 channels. */
void bt_theme_unpack(guint32 rgb, gdouble *r, gdouble *g, gdouble *b);
void bt_theme_unpack_f(guint32 rgb, float *r, float *g, float *b);

/* WCAG 2.x relative luminance and contrast ratio (1..21). */
gdouble bt_theme_luminance(guint32 rgb);
gdouble bt_theme_contrast(guint32 fg, guint32 bg);

/* Parses "#RRGGBB" (case-insensitive). */
gboolean bt_theme_parse_hex(const gchar *hex, guint32 *rgb_out);

/* ANSI 16-color palette (0..7 normal, 8..15 bright), derived from the tokens. */
guint32 bt_theme_ansi(guint idx);

/* Terminal & link colors. */
guint32 bt_theme_terminal_selection(void);   /* accent over surface-1, opaque */
guint32 bt_theme_terminal_search_hit(void);  /* warning over surface-1, opaque */
guint32 bt_theme_blend(guint32 fg, guint32 bg, gdouble alpha);

const gchar *bt_scheme_to_string(BtUiScheme scheme);
BtUiScheme bt_scheme_from_string(const gchar *str);

/* Font stacks (kept identical to theme.css). */
const gchar *bt_theme_default_mono_font(void);
const gchar *bt_theme_ui_font_stack(void);

G_END_DECLS

#endif
