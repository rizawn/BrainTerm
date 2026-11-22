/* theme.h — BrainTerm Monochrome ("Ink") Palette Tokens & Scheme Controller (M-UI). */
#ifndef BT_THEME_H
#define BT_THEME_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BT_SCHEME_DARK = 0,   /* default: dark-first per primary mock */
  BT_SCHEME_LIGHT = 1,
  BT_SCHEME_SYSTEM = 2
} BtUiScheme;

/* Exact 8 pure-grayscale Ink tokens per mode (R == G == B). */
typedef struct {
  guint32 surface;
  guint32 raised;
  guint32 ink;
  guint32 bright;
  guint32 mid;
  guint32 faint;
  guint32 hairline;
  guint32 deep;
} BtInkPalette;

const BtInkPalette *bt_theme_palette(gboolean dark);

const gchar *bt_scheme_to_string(BtUiScheme scheme);
BtUiScheme bt_scheme_from_string(const gchar *str);

/* Resolves the preferred monospace font string for editor + terminal (§H.16). */
const gchar *bt_theme_default_mono_font(void);

/* Verifies that a #RRGGBB hex string is strictly achromatic (R == G == B). */
gboolean bt_hex_is_grayscale(const gchar *hex, guint8 *lum_out);

G_END_DECLS

#endif
