/* mathrender.c — Cairo-FT math renderer & surface cache (M4). */
#include "ui/mathrender.h"

#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <string.h>

#define BT_MATH_CACHE_CAP 512u

typedef struct {
  guint64 key_hash;
  BtMathRendered rendered;
  guint64 last_used_tick;
} CacheEntry;

struct BtMathRenderer {
  BtMathFont *font;
  gboolean owns_font;
  FT_Library ft_lib;
  FT_Face ft_face;
  cairo_font_face_t *cairo_face;
  GHashTable *cache; /* guint64* -> CacheEntry* */
  guint64 tick;
};

static void cache_entry_free(gpointer data)
{
  CacheEntry *e = data;
  if (e == NULL)
    return;
  if (e->rendered.surface != NULL)
    cairo_surface_destroy(e->rendered.surface);
  g_free(e);
}

BtMathRenderer *bt_math_renderer_new(BtMathFont *font)
{
  if (font == NULL)
    font = bt_math_font_get_default();
  if (font == NULL)
    return NULL;

  GBytes *bytes = bt_math_font_get_bytes(font);
  if (bytes == NULL)
    return NULL;

  gsize size = 0;
  const FT_Byte *data = g_bytes_get_data(bytes, &size);
  if (data == NULL || size == 0)
    return NULL;

  FT_Library ft_lib = NULL;
  if (FT_Init_FreeType(&ft_lib) != 0)
    return NULL;

  FT_Face ft_face = NULL;
  if (FT_New_Memory_Face(ft_lib, data, (FT_Long)size, 0, &ft_face) != 0) {
    FT_Done_FreeType(ft_lib);
    return NULL;
  }

  cairo_font_face_t *cface = cairo_ft_font_face_create_for_ft_face(ft_face, 0);
  BtMathRenderer *r = g_new0(BtMathRenderer, 1);
  r->font = font;
  r->owns_font = FALSE;
  r->ft_lib = ft_lib;
  r->ft_face = ft_face;
  r->cairo_face = cface;
  r->cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, cache_entry_free);
  r->tick = 1;
  return r;
}

BtMathRenderer *bt_math_renderer_get_default(void)
{
  static BtMathRenderer *shared = NULL;
  if (shared == NULL)
    shared = bt_math_renderer_new(bt_math_font_get_default());
  return shared;
}

void bt_math_renderer_free(BtMathRenderer *r)
{
  if (r == NULL)
    return;
  if (r->cache != NULL)
    g_hash_table_destroy(r->cache);
  if (r->cairo_face != NULL)
    cairo_font_face_destroy(r->cairo_face);
  if (r->ft_face != NULL)
    FT_Done_Face(r->ft_face);
  if (r->ft_lib != NULL)
    FT_Done_FreeType(r->ft_lib);
  if (r->owns_font && r->font != NULL)
    bt_math_font_free(r->font);
  g_free(r);
}

void bt_math_renderer_clear_cache(BtMathRenderer *r)
{
  if (r != NULL && r->cache != NULL)
    g_hash_table_remove_all(r->cache);
}

void bt_math_renderer_draw_layout(BtMathRenderer *r, cairo_t *cr,
                                  const BtMathLayout *layout,
                                  double origin_x, double baseline_y,
                                  double font_px,
                                  double fg_r, double fg_g, double fg_b, double fg_a)
{
  if (r == NULL || cr == NULL || layout == NULL || r->cairo_face == NULL)
    return;

  cairo_save(cr);
  cairo_set_font_face(cr, r->cairo_face);

  for (guint i = 0; i < layout->ops->len; i++) {
    const BtDrawOp *op = &g_array_index(layout->ops, BtDrawOp, i);
    if (op->is_error)
      cairo_set_source_rgba(cr, 0.90, 0.42, 0.42, fg_a);
    else
      cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, fg_a);

    if (op->kind == BT_DRAW_GLYPH) {
      double px_size = MAX(font_px * op->u.glyph.scale, 4.0);
      cairo_set_font_size(cr, px_size);
      cairo_glyph_t cg = {
        .index = op->u.glyph.glyph_id,
        .x = origin_x + op->u.glyph.x_em * font_px,
        .y = baseline_y + op->u.glyph.y_em * font_px,
      };
      cairo_show_glyphs(cr, &cg, 1);
    } else if (op->kind == BT_DRAW_RULE) {
      double rx = origin_x + op->u.rule.x_em * font_px;
      double ry = baseline_y + op->u.rule.y_em * font_px;
      double rw = MAX(op->u.rule.w_em * font_px, 1.0);
      double rh = MAX(op->u.rule.h_em * font_px, 1.15);
      cairo_rectangle(cr, rx, ry, rw, rh);
      cairo_fill(cr);
    }
  }

  cairo_restore(cr);
}

static guint64 compute_cache_key(const char *latex, gsize len, BtMathMode mode,
                                 double font_px, int device_scale)
{
  guint64 h = 1469598103934665603ULL;
  for (gsize i = 0; i < len; i++) {
    h ^= (guint8)latex[i];
    h *= 1099511628211ULL;
  }
  guint64 px_q = (guint64)llround(font_px * 10.0);
  h ^= (guint64)mode * 0x9E3779B97F4A7C15ULL;
  h *= 1099511628211ULL;
  h ^= px_q;
  h *= 1099511628211ULL;
  h ^= (guint64)MAX(device_scale, 1);
  return h;
}

static void evict_lru_if_needed(BtMathRenderer *r)
{
  if (g_hash_table_size(r->cache) < BT_MATH_CACHE_CAP)
    return;
  GHashTableIter iter;
  gpointer k = NULL, v = NULL;
  guint64 oldest_tick = G_MAXUINT64;
  gpointer oldest_key = NULL;
  g_hash_table_iter_init(&iter, r->cache);
  while (g_hash_table_iter_next(&iter, &k, &v)) {
    CacheEntry *e = v;
    if (e->last_used_tick < oldest_tick) {
      oldest_tick = e->last_used_tick;
      oldest_key = k;
    }
  }
  if (oldest_key != NULL)
    g_hash_table_remove(r->cache, oldest_key);
}

const BtMathRendered *bt_math_renderer_get_cached(BtMathRenderer *r,
                                                  const char *latex, gsize len,
                                                  BtMathMode mode,
                                                  double font_px, int device_scale)
{
  if (r == NULL || latex == NULL)
    return NULL;
  if (device_scale < 1)
    device_scale = 1;

  guint64 key = compute_cache_key(latex, len, mode, font_px, device_scale);
  CacheEntry *found = g_hash_table_lookup(r->cache, &key);
  if (found != NULL) {
    found->last_used_tick = ++r->tick;
    return &found->rendered;
  }

  BtMathParseResult *ast = bt_math_parse(latex, len, mode);
  BtMathLayout *lay = bt_math_layout_build(r->font, ast);
  guint err_cnt = ast && ast->errors ? ast->errors->len : 0;
  bt_math_parse_result_free(ast);
  if (lay == NULL)
    return NULL;

  double pad_x = 3.0;
  double pad_y = 2.0;
  double asc_px = lay->ascent_em * font_px;
  double desc_px = lay->descent_em * font_px;
  double w_px = ceil(lay->width_em * font_px + pad_x * 2.0);
  double h_px = ceil(asc_px + desc_px + pad_y * 2.0);
  w_px = CLAMP(w_px, 6.0, 2400.0);
  h_px = CLAMP(h_px, 12.0, 1600.0);
  double base_y = pad_y + asc_px;

  int surf_w = (int)ceil(w_px * (double)device_scale);
  int surf_h = (int)ceil(h_px * (double)device_scale);
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
  cairo_surface_set_device_scale(surf, (double)device_scale, (double)device_scale);

  cairo_t *cr = cairo_create(surf);
  /* Dusk theme primary ink #E6E8EC */
  bt_math_renderer_draw_layout(r, cr, lay, pad_x, base_y, font_px,
                               0.902, 0.910, 0.925, 1.0);
  cairo_destroy(cr);
  bt_math_layout_free(lay);

  evict_lru_if_needed(r);
  CacheEntry *entry = g_new0(CacheEntry, 1);
  entry->key_hash = key;
  entry->last_used_tick = ++r->tick;
  entry->rendered.surface = surf;
  entry->rendered.width_px = w_px;
  entry->rendered.height_px = h_px;
  entry->rendered.baseline_y_px = base_y;
  entry->rendered.ascent_px = asc_px;
  entry->rendered.descent_px = desc_px;
  entry->rendered.error_count = err_cnt;

  guint64 *kptr = g_new(guint64, 1);
  *kptr = key;
  g_hash_table_insert(r->cache, kptr, entry);
  return &entry->rendered;
}
