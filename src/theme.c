/* theme.c — design-token helpers (contrast math, ANSI palette, scheme names). */
#include "theme.h"

#include <math.h>
#include <string.h>

void bt_theme_unpack(guint32 rgb, gdouble *r, gdouble *g, gdouble *b)
{
  if (r != NULL)
    *r = (gdouble)((rgb >> 16) & 0xFFu) / 255.0;
  if (g != NULL)
    *g = (gdouble)((rgb >> 8) & 0xFFu) / 255.0;
  if (b != NULL)
    *b = (gdouble)(rgb & 0xFFu) / 255.0;
}

static gdouble lin(gdouble c)
{
  return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

gdouble bt_theme_luminance(guint32 rgb)
{
  gdouble r, g, b;
  bt_theme_unpack(rgb, &r, &g, &b);
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

gdouble bt_theme_contrast(guint32 fg, guint32 bg)
{
  gdouble a = bt_theme_luminance(fg);
  gdouble b = bt_theme_luminance(bg);
  gdouble hi = MAX(a, b), lo = MIN(a, b);
  return (hi + 0.05) / (lo + 0.05);
}

static gint hex_nibble(gchar c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

gboolean bt_theme_parse_hex(const gchar *hex, guint32 *rgb_out)
{
  if (hex == NULL || hex[0] != '#' || strlen(hex) < 7)
    return FALSE;
  guint32 v = 0;
  for (int i = 1; i <= 6; i++) {
    gint n = hex_nibble(hex[i]);
    if (n < 0)
      return FALSE;
    v = (v << 4) | (guint32)n;
  }
  if (rgb_out != NULL)
    *rgb_out = v;
  return TRUE;
}

static const guint32 ANSI[16] = {
  /* normal */
  BT_COL_SURFACE_2, BT_COL_DANGER, BT_COL_SUCCESS, BT_COL_WARNING,
  0x7AA2F7u, BT_COL_ACCENT, 0x5EEAD4u, 0xC8CCD4u,
  /* bright */
  BT_COL_TEXT_MUTED, 0xFCA5A5u, 0x86EFACu, 0xFDE68Au,
  0x93C5FDu, 0xB4A9FAu, 0x99F6E4u, BT_COL_TEXT_PRIMARY,
};

guint32 bt_theme_ansi(guint idx)
{
  return ANSI[idx & 15u];
}

static guint32 blend(guint32 fg, guint32 bg, gdouble a)
{
  guint32 out = 0;
  for (int sh = 16; sh >= 0; sh -= 8) {
    gdouble f = (gdouble)((fg >> sh) & 0xFFu);
    gdouble b = (gdouble)((bg >> sh) & 0xFFu);
    out |= ((guint32)lround(f * a + b * (1.0 - a)) & 0xFFu) << sh;
  }
  return out;
}

guint32 bt_theme_terminal_selection(void)
{
  return blend(BT_COL_ACCENT, BT_COL_SURFACE_1, 0.38);
}

guint32 bt_theme_terminal_search_hit(void)
{
  return blend(BT_COL_WARNING, BT_COL_SURFACE_1, 0.28);
}

const gchar *bt_scheme_to_string(BtUiScheme scheme)
{
  switch (scheme) {
  case BT_SCHEME_LIGHT:  return "light";
  case BT_SCHEME_SYSTEM: return "system";
  case BT_SCHEME_DARK:
  default:               return "dark";
  }
}

BtUiScheme bt_scheme_from_string(const gchar *str)
{
  if (str == NULL)
    return BT_SCHEME_DARK;
  if (g_ascii_strcasecmp(str, "light") == 0)
    return BT_SCHEME_LIGHT;
  if (g_ascii_strcasecmp(str, "system") == 0)
    return BT_SCHEME_SYSTEM;
  return BT_SCHEME_DARK;
}

const gchar *bt_theme_default_mono_font(void)
{
  return "JetBrains Mono, Cascadia Mono, Consolas, Monospace 13";
}

const gchar *bt_theme_ui_font_stack(void)
{
  return "Inter, Inter Variable, Segoe UI Variable, Segoe UI, sans-serif";
}
