/* platform.h — the few OS primitives the portable core needs.
 * Implemented in platform_win.c (Win32) and platform_unix.c (POSIX). */
#ifndef BT_PLATFORM_H
#define BT_PLATFORM_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BT_DIRENT_FILE,
  BT_DIRENT_DIR,
  BT_DIRENT_LINK, /* symlink / junction: never followed by the scanner */
  BT_DIRENT_OTHER
} BtDirentKind;

typedef struct {
  gchar *name; /* UTF-8 basename */
  BtDirentKind kind;
} BtDirent;

void bt_dirent_free(gpointer dirent);

/* Lists one directory without following links. Returns a GPtrArray of
 * BtDirent* (owned by caller), or NULL with error set. */
GPtrArray *bt_platform_list_dir(const gchar *dir, GError **error);

/* Resolves every link in an EXISTING path; returns a new absolute UTF-8
 * path or NULL with error set. */
gchar *bt_platform_realpath(const gchar *path, GError **error);

/* Atomically replaces dst with src (same directory). */
gboolean bt_platform_replace_file(const gchar *src, const gchar *dst, GError **error);

/* Atomically creates dst from src (same directory) failing without overwrite if dst exists. */
gboolean bt_platform_create_file_only(const gchar *src, const gchar *dst,
                                      gboolean *out_already_exists, GError **error);

/* Flushes directory metadata (POSIX); no-op on Windows where the rename is
 * issued with MOVEFILE_WRITE_THROUGH. */
gboolean bt_platform_fsync_dir(const gchar *dir, GError **error);

G_END_DECLS

#endif
