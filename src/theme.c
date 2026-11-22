/* theme.c — Monochrome Ink palette tokens and helpers (M-UI). */
#include "theme.h"

#include <string.h>

static const BtInkPalette INK_DARK = {
  .surface  = 0x000B0B0Bu,
  .raised   = 0x00101010u,
  .ink      = 0x00ECECECu,
  .bright   = 0x00FAFAFAu,
  .mid      = 0x008B8B8Bu,
  .faint    = 0x006E6E6Eu,
  .hairline = 0x001C1C1Cu,
  .deep     = 0x00000000u,
};

static const BtInkPalette INK_LIGHT = {
  .surface  = 0x00FFFFFFu,
  .raised   = 0x00F5F5F5u,
  .ink      = 0x001A1A1Au,
  .bright   = 0x00000000u,
  .mid      = 0x00757575u,
  .faint    = 0x00B0B0B0u,
  .hairline = 0x00E0E0E0u,
  .deep     = 0x00ECECECu,
};

const BtInkPalette *bt_theme_palette(gboolean dark)
{
  return dark ? &INK_DARK : &INK_LIGHT;
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
#ifdef G_OS_WIN32
  return "Cascadia Mono, JetBrains Mono, Consolas, Monospace 11";
#else
  return "JetBrains Mono, Cascadia Mono, Monospace 11";
#endif
}

static gint hex_nibble(gchar c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

gboolean bt_hex_is_grayscale(const gchar *hex, guint8 *lum_out)
{
  if (hex == NULL || hex[0] != '#' || strlen(hex) != 7)
    return FALSE;
  gint r1 = hex_nibble(hex[1]), r2 = hex_nibble(hex[2]);
  gint g1 = hex_nibble(hex[3]), g2 = hex_nibble(hex[4]);
  gint b1 = hex_nibble(hex[5]), b2 = hex_nibble(hex[6]);
  if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0)
    return FALSE;
  guint8 r = (guint8)((r1 << 4) | r2);
  guint8 g = (guint8)((g1 << 4) | g2);
  guint8 b = (guint8)((b1 << 4) | b2);
  if (r != g || g != b)
    return FALSE;
  if (lum_out != NULL)
    *lum_out = r;
  return TRUE;
}
