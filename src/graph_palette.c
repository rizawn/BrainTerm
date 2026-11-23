/* graph_palette.c — Pure-C Andromeda galaxy palette, folder color assignment,
 * descendant family variation (+-8%), and JSON persistence. */
#include "graph_palette.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <string.h>

static const BtGalaxySwatch GALAXY_SWATCHES[BT_GALAXY_PALETTE_COUNT] = {
  {"Core Gold",      0xFFC857u},
  {"Ember",          0xFF8A3Du},
  {"Rose Dust",      0xFF7AA2u},
  {"Nebula Magenta", 0xE64AA8u},
  {"Dusty Lilac",    0xB58CE6u},
  {"Violet Haze",    0x8B6CF0u},
  {"Indigo Arm",     0x5B7CFAu},
  {"Andromeda Blue", 0x4FA3FFu},
  {"Ice Cyan",       0x5EE0F2u},
  {"Teal Plasma",    0x2FD6B5u},
  {"Oxygen Green",   0x7BE495u},
  {"Pale Star",      0xBFD7FFu},
};

/* Stride-5 permutation of 0..11 maximizes hue distance between adjacent folders. */
static const guint MAX_DIST_ORDER[BT_GALAXY_PALETTE_COUNT] = {
  0u, 7u, 3u, 9u, 1u, 6u, 10u, 4u, 8u, 2u, 5u, 11u
};

const BtGalaxySwatch *bt_galaxy_palette(guint *count_out)
{
  if (count_out != NULL)
    *count_out = BT_GALAXY_PALETTE_COUNT;
  return GALAXY_SWATCHES;
}

static guint64 splitmix64(guint64 *s)
{
  guint64 z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static gdouble rng_unit(guint64 *s)
{
  return (gdouble)(splitmix64(s) >> 11) * (1.0 / 9007199254740992.0);
}

static guint32 hsv_to_rgb(gdouble h_deg, gdouble s, gdouble v)
{
  gdouble h = fmod(h_deg, 360.0);
  if (h < 0.0) h += 360.0;
  s = CLAMP(s, 0.0, 1.0);
  v = CLAMP(v, 0.0, 1.0);
  gdouble c = v * s;
  gdouble x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
  gdouble m = v - c;
  gdouble r = 0.0, g = 0.0, b = 0.0;
  if (h < 60.0)       { r = c; g = x; b = 0.0; }
  else if (h < 120.0) { r = x; g = c; b = 0.0; }
  else if (h < 180.0) { r = 0.0; g = c; b = x; }
  else if (h < 240.0) { r = 0.0; g = x; b = c; }
  else if (h < 300.0) { r = x; g = 0.0; b = c; }
  else                { r = c; g = 0.0; b = x; }
  guint ri = (guint)CLAMP(llround((r + m) * 255.0), 0, 255);
  guint gi = (guint)CLAMP(llround((g + m) * 255.0), 0, 255);
  guint bi = (guint)CLAMP(llround((b + m) * 255.0), 0, 255);
  return (ri << 16) | (gi << 8) | bi;
}

static void rgb_to_hsv(guint32 rgb, gdouble *h_out, gdouble *s_out, gdouble *v_out)
{
  gdouble r = (gdouble)((rgb >> 16) & 0xFFu) / 255.0;
  gdouble g = (gdouble)((rgb >> 8) & 0xFFu) / 255.0;
  gdouble b = (gdouble)(rgb & 0xFFu) / 255.0;
  gdouble mx = MAX(r, MAX(g, b));
  gdouble mn = MIN(r, MIN(g, b));
  gdouble d = mx - mn;
  gdouble h = 0.0;
  if (d > 1e-6) {
    if (mx == r)      h = 60.0 * fmod((g - b) / d, 6.0);
    else if (mx == g) h = 60.0 * ((b - r) / d + 2.0);
    else              h = 60.0 * ((r - g) / d + 4.0);
    if (h < 0.0) h += 360.0;
  }
  if (h_out) *h_out = h;
  if (s_out) *s_out = (mx > 1e-6) ? (d / mx) : 0.0;
  if (v_out) *v_out = mx;
}

gdouble bt_color_perceptual_dist(guint32 a, guint32 b)
{
  gdouble r1 = (gdouble)((a >> 16) & 0xFFu);
  gdouble g1 = (gdouble)((a >> 8) & 0xFFu);
  gdouble b1 = (gdouble)(a & 0xFFu);
  gdouble r2 = (gdouble)((b >> 16) & 0xFFu);
  gdouble g2 = (gdouble)((b >> 8) & 0xFFu);
  gdouble b2 = (gdouble)(b & 0xFFu);
  gdouble rmean = 0.5 * (r1 + r2);
  gdouble dr = r1 - r2;
  gdouble dg = g1 - g2;
  gdouble db = b1 - b2;
  return sqrt((2.0 + rmean / 256.0) * dr * dr + 4.0 * dg * dg +
              (2.0 + (255.0 - rmean) / 256.0) * db * db);
}

guint32 bt_folder_colors_generate_distinct(const guint32 *existing, guint existing_count,
                                           guint64 *rng_state)
{
  /* Galaxy-ish hue bands: gold/orange 15-50, teal 160-185, blue/violet 200-260, magenta 270-340 */
  static const gdouble bands[4][2] = {
    {15.0, 50.0},
    {160.0, 185.0},
    {200.0, 260.0},
    {270.0, 340.0},
  };
  guint64 local_seed = rng_state ? *rng_state : 0x517CC1B727220A95ULL;
  guint32 best_col = 0x5EE0F2u;
  gdouble best_min_d = -1.0;

  for (guint attempt = 0; attempt < 64u; attempt++) {
    guint b = (guint)(splitmix64(&local_seed) & 3u);
    gdouble h = bands[b][0] + rng_unit(&local_seed) * (bands[b][1] - bands[b][0]);
    gdouble s = 0.55 + rng_unit(&local_seed) * 0.30; /* 55-85% */
    gdouble v = 0.85 + rng_unit(&local_seed) * 0.15; /* 85-100% */
    guint32 cand = hsv_to_rgb(h, s, v);

    gdouble min_d = 1e9;
    for (guint i = 0; i < existing_count; i++) {
      gdouble d = bt_color_perceptual_dist(cand, existing[i]);
      if (d < min_d)
        min_d = d;
    }
    if (min_d > best_min_d) {
      best_min_d = min_d;
      best_col = cand;
    }
    if (min_d >= 95.0)
      break;
  }

  if (rng_state != NULL)
    *rng_state = local_seed;
  return best_col;
}

static gboolean parse_hex_rgb(const gchar *hex, guint32 *out)
{
  if (hex == NULL)
    return FALSE;
  if (hex[0] == '#')
    hex++;
  if (strlen(hex) != 6u)
    return FALSE;
  gchar *end = NULL;
  guint64 val = g_ascii_strtoull(hex, &end, 16);
  if (end == NULL || *end != '\0')
    return FALSE;
  if (out != NULL)
    *out = (guint32)(val & 0xFFFFFFu);
  return TRUE;
}

BtFolderColors *bt_folder_colors_load(const gchar *vault_root)
{
  BtFolderColors *fc = g_new0(BtFolderColors, 1);
  fc->map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (vault_root != NULL && vault_root[0] != '\0')
    fc->json_path = g_build_filename(vault_root, ".brainterm", "folder-colors.json", NULL);

  if (fc->json_path != NULL && g_file_test(fc->json_path, G_FILE_TEST_IS_REGULAR)) {
    g_autofree gchar *raw = NULL;
    gsize len = 0;
    if (g_file_get_contents(fc->json_path, &raw, &len, NULL) && raw != NULL) {
      cJSON *root = cJSON_ParseWithLength(raw, len);
      if (cJSON_IsObject(root)) {
        for (cJSON *it = root->child; it != NULL; it = it->next) {
          if (it->string != NULL && cJSON_IsString(it)) {
            guint32 rgb = 0;
            if (parse_hex_rgb(it->valuestring, &rgb))
              bt_folder_colors_set(fc, it->string, rgb);
          }
        }
      }
      cJSON_Delete(root);
    }
  }
  return fc;
}

void bt_folder_colors_free(BtFolderColors *fc)
{
  if (fc == NULL)
    return;
  if (fc->map != NULL)
    g_hash_table_destroy(fc->map);
  g_free(fc->json_path);
  g_free(fc);
}

gboolean bt_folder_colors_save(const BtFolderColors *fc, GError **error)
{
  if (fc == NULL || fc->json_path == NULL)
    return FALSE;
  g_autofree gchar *dir = g_path_get_dirname(fc->json_path);
  if (g_mkdir_with_parents(dir, 0755) != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Cannot create %s", dir);
    return FALSE;
  }

  cJSON *root = cJSON_CreateObject();
  GHashTableIter it;
  gpointer k = NULL, v = NULL;
  g_hash_table_iter_init(&it, fc->map);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    guint32 rgb = GPOINTER_TO_UINT(v) & 0xFFFFFFu;
    gchar hex[10];
    g_snprintf(hex, sizeof(hex), "#%06X", rgb);
    cJSON_AddStringToObject(root, (const gchar *)k, hex);
  }
  gchar *json = cJSON_Print(root);
  cJSON_Delete(root);
  if (json == NULL)
    return FALSE;

  gboolean ok = g_file_set_contents(fc->json_path, json, -1, error);
  cJSON_free(json);
  return ok;
}

guint32 bt_folder_colors_get(const BtFolderColors *fc, const gchar *top_folder)
{
  if (fc == NULL || fc->map == NULL || top_folder == NULL || top_folder[0] == '\0')
    return 0u;
  gpointer v = g_hash_table_lookup(fc->map, top_folder);
  return v != NULL ? (GPOINTER_TO_UINT(v) & 0xFFFFFFu) : 0u;
}

void bt_folder_colors_set(BtFolderColors *fc, const gchar *top_folder, guint32 rgb)
{
  if (fc == NULL || fc->map == NULL || top_folder == NULL || top_folder[0] == '\0')
    return;
  g_hash_table_replace(fc->map, g_strdup(top_folder),
                       GUINT_TO_POINTER((rgb & 0xFFFFFFu) | 0xFF000000u));
}

static guint32 vary_descendant_color(guint32 base_rgb, const gchar *path)
{
  if (path == NULL)
    return base_rgb;
  guint32 h = 2166136261u;
  for (const gchar *p = path; *p != '\0'; p++) {
    h ^= (guchar)*p;
    h *= 16777619u;
  }
  gdouble u1 = (gdouble)(h & 0xFFu) / 255.0;         /* [0, 1] */
  gdouble u2 = (gdouble)((h >> 8) & 0xFFu) / 255.0;  /* [0, 1] */
  gdouble hue = 0.0, sat = 0.0, val = 0.0;
  rgb_to_hsv(base_rgb, &hue, &sat, &val);
  /* +-8% variation in saturation & value so subfolders stay clearly in-family. */
  sat = CLAMP(sat + (u1 * 2.0 - 1.0) * 0.08, 0.35, 1.0);
  val = CLAMP(val + (u2 * 2.0 - 1.0) * 0.08, 0.65, 1.0);
  return hsv_to_rgb(hue, sat, val);
}

static gint cmp_str_ptr(gconstpointer a, gconstpointer b)
{
  const gchar *sa = *(const gchar *const *)a;
  const gchar *sb = *(const gchar *const *)b;
  return g_ascii_strcasecmp(sa, sb);
}

gboolean bt_folder_colors_apply(BtFolderColors *fc, BtGraph *g, gboolean color_by_folder,
                                guint64 rng_seed)
{
  if (g == NULL || g->nodes == NULL)
    return FALSE;

  if (!color_by_folder) {
    for (guint i = 0; i < g->nodes->len; i++) {
      BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
      nd->color_rgb = 0u; /* fall back to kind_rgb() */
    }
    return FALSE;
  }

  /* Collect unique top-level folders in sorted order. */
  GPtrArray *folders = g_ptr_array_new();
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < g->nodes->len; i++) {
    const BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->top_folder != NULL && nd->top_folder[0] != '\0' &&
        !g_hash_table_contains(seen, nd->top_folder)) {
      g_hash_table_add(seen, nd->top_folder);
      g_ptr_array_add(folders, nd->top_folder);
    }
  }
  g_ptr_array_sort(folders, cmp_str_ptr);
  g_hash_table_destroy(seen);

  gboolean added = FALSE;
  gboolean had_existing = (fc != NULL && g_hash_table_size(fc->map) > 0u);
  guint64 rng = rng_seed ? rng_seed : (guint64)g_get_real_time();

  /* Track currently used colors. */
  GArray *used_cols = g_array_new(FALSE, FALSE, sizeof(guint32));
  if (fc != NULL) {
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&it, fc->map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      guint32 c = GPOINTER_TO_UINT(v) & 0xFFFFFFu;
      g_array_append_val(used_cols, c);
    }
  }

  for (guint fi = 0; fi < folders->len; fi++) {
    const gchar *fname = g_ptr_array_index(folders, fi);
    guint32 col = bt_folder_colors_get(fc, fname);
    if (col == 0u) {
      /* Build list of unused palette indices. */
      guint unused[BT_GALAXY_PALETTE_COUNT];
      guint unused_n = 0u;
      for (guint s = 0; s < BT_GALAXY_PALETTE_COUNT; s++) {
        guint idx = MAX_DIST_ORDER[s];
        guint32 sw = GALAXY_SWATCHES[idx].rgb;
        gboolean in_use = FALSE;
        for (guint u = 0; u < used_cols->len; u++) {
          if (g_array_index(used_cols, guint32, u) == sw) {
            in_use = TRUE;
            break;
          }
        }
        if (!in_use)
          unused[unused_n++] = idx;
      }

      if (unused_n > 0u) {
        /* Initial bulk assignment maximizes hue distance; NEW folder picks random unused. */
        guint pick = had_existing ? unused[(guint)(splitmix64(&rng) % unused_n)] : unused[0];
        col = GALAXY_SWATCHES[pick].rgb;
      } else {
        col = bt_folder_colors_generate_distinct((const guint32 *)used_cols->data,
                                                 used_cols->len, &rng);
      }
      if (fc != NULL)
        bt_folder_colors_set(fc, fname, col);
      g_array_append_val(used_cols, col);
      added = TRUE;
    }
  }

  /* Apply colors to every node in graph. */
  for (guint i = 0; i < g->nodes->len; i++) {
    BtGraphNode *nd = g_ptr_array_index(g->nodes, i);
    if (nd->kind == BT_NODE_TAG || nd->kind == BT_NODE_SESSION ||
        nd->kind == BT_NODE_IMPORT_ROOT || nd->kind == BT_NODE_GHOST) {
      nd->color_rgb = 0u; /* Type shapes keep subtle muted/accent defaults */
      continue;
    }
    if (nd->top_folder == NULL || nd->top_folder[0] == '\0') {
      nd->color_rgb = BT_GALAXY_ROOT_COLOR;
    } else {
      guint32 base = bt_folder_colors_get(fc, nd->top_folder);
      if (base == 0u)
        base = BT_GALAXY_ROOT_COLOR;
      if (nd->kind == BT_NODE_FOLDER && nd->parent_idx < 0)
        nd->color_rgb = base; /* top-level parent gets exact palette color */
      else
        nd->color_rgb = vary_descendant_color(base, nd->relpath);
    }
  }

  g_array_unref(used_cols);
  g_ptr_array_unref(folders);
  return added;
}
