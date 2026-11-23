/* graph_palette.h — Pure-C (GTK-free) Galaxy folder color palette & persistence.
 * Assigns distinct Andromeda-inspired colors to top-level vault folders,
 * varies descendant lightness/saturation +-8%, and persists to
 * <vault>/.brainterm/folder-colors.json. */
#ifndef BT_GRAPH_PALETTE_H
#define BT_GRAPH_PALETTE_H

#include <glib.h>

#include "graph.h"

G_BEGIN_DECLS

#define BT_GALAXY_PALETTE_COUNT 12u
#define BT_GALAXY_ROOT_COLOR    0xBFD7FFu /* Pale Star for vault-root notes */
#define BT_GALAXY_BG_TOP        0x05070Du
#define BT_GALAXY_BG_BOTTOM     0x02030Au

typedef struct {
  const gchar *name;
  guint32 rgb;
} BtGalaxySwatch;

typedef struct {
  GHashTable *map; /* top_folder (gchar*) -> GUINT_TO_POINTER(rgb | 0xFF000000u) */
  gchar *json_path;
} BtFolderColors;

/* Returns pointer to the static 12-swatch Andromeda galaxy palette. */
const BtGalaxySwatch *bt_galaxy_palette(guint *count_out);

/* Loads <vault_root>/.brainterm/folder-colors.json (or creates empty map if absent). */
BtFolderColors *bt_folder_colors_load(const gchar *vault_root);
void bt_folder_colors_free(BtFolderColors *fc);

/* Persists current folder -> #RRGGBB map atomically to disk. */
gboolean bt_folder_colors_save(const BtFolderColors *fc, GError **error);

/* Looks up existing color (returns 0 if unassigned). */
guint32 bt_folder_colors_get(const BtFolderColors *fc, const gchar *top_folder);

/* Sets/overrides color for a top-level folder. */
void bt_folder_colors_set(BtFolderColors *fc, const gchar *top_folder, guint32 rgb);

/* Assigns colors for all top-level folders in graph:
 *   - Initial bulk assignment maximizes hue distance between neighboring folders.
 *   - New folders get a random unused swatch; once exhausted, generates a galaxy-band
 *     color perceptually distant from all existing folder colors.
 *   - Writes color_rgb on every folder & note node (descendants get +-8% L/S variation).
 * Returns TRUE if any new folder color was added (caller should save). */
gboolean bt_folder_colors_apply(BtFolderColors *fc, BtGraph *g, gboolean color_by_folder,
                                guint64 rng_seed);

/* Perceptual color distance (redmean-weighted Euclidean in [0..765]). */
gdouble bt_color_perceptual_dist(guint32 a, guint32 b);

/* Generates a new galaxy-band color far from existing_colors[0..existing_count-1]. */
guint32 bt_folder_colors_generate_distinct(const guint32 *existing, guint existing_count,
                                           guint64 *rng_state);

G_END_DECLS

#endif
