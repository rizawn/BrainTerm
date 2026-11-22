/* themeload.h — loads theme.css (GResource or BRAINTERM_CSS_DEV with hot reload),
 * registers the symbolic icon set and bundled fonts. UI layer only. */
#ifndef BT_THEMELOAD_H
#define BT_THEMELOAD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define BT_THEME_RESOURCE_PATH "/local/brainterm/theme.css"
#define BT_ICON_RESOURCE_PATH  "/local/brainterm/icons"
#define BT_CSS_DEV_ENV         "BRAINTERM_CSS_DEV"

typedef struct BtThemeLoader BtThemeLoader;

/* Registers bundled Inter / JetBrains Mono (fontconfig and/or Win32 private
 * fonts). Must run BEFORE gtk_init so the font map sees them. Idempotent. */
void bt_fonts_register(void);

/* TRUE if Pango can resolve a family with this name (case-insensitive). */
gboolean bt_fonts_family_available(const gchar *family);

/* Installs the CSS provider at PRIORITY_APPLICATION on `display` (NULL = default),
 * adds the icon resource path, and — when BRAINTERM_CSS_DEV is set — watches
 * that file and hot-reloads it. */
BtThemeLoader *bt_themeloader_new(GdkDisplay *display);
void bt_themeloader_free(BtThemeLoader *loader);

/* Number of CSS parse errors seen so far (0 for a healthy theme). */
guint bt_themeloader_error_count(const BtThemeLoader *loader);
/* TRUE when the stylesheet comes from BRAINTERM_CSS_DEV instead of GResource. */
gboolean bt_themeloader_is_dev(const BtThemeLoader *loader);

/* Directory containing the executable (caller frees), NULL if unknown. */
gchar *bt_themeload_exe_dir(void);

G_END_DECLS

#endif
