/* platform_unix.c — POSIX implementations of platform.h. */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "bterror.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void bt_dirent_free(gpointer dirent)
{
  BtDirent *d = dirent;
  if (d == NULL)
    return;
  g_free(d->name);
  g_free(d);
}

static void set_errno_error(GError **error, const gchar *what, const gchar *path, int err)
{
  g_set_error(error, BT_ERROR, err == ENOENT ? BT_ERROR_NOT_FOUND : BT_ERROR_IO, "%s '%s': %s",
              what, path, g_strerror(err));
}

GPtrArray *bt_platform_list_dir(const gchar *dir, GError **error)
{
  DIR *d = opendir(dir);
  if (d == NULL) {
    set_errno_error(error, "Cannot list directory", dir, errno);
    return NULL;
  }
  GPtrArray *out = g_ptr_array_new_with_free_func(bt_dirent_free);
  for (;;) {
    errno = 0;
    struct dirent *e = readdir(d);
    if (e == NULL) {
      if (errno != 0) {
        set_errno_error(error, "Error while listing directory", dir, errno);
        g_ptr_array_unref(out);
        closedir(d);
        return NULL;
      }
      break;
    }
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    gchar *full = g_build_filename(dir, e->d_name, NULL);
    struct stat st;
    int rc = lstat(full, &st);
    g_free(full);
    if (rc != 0)
      continue; /* vanished between readdir and lstat */
    BtDirent *de = g_new0(BtDirent, 1);
    de->name = g_strdup(e->d_name);
    if (S_ISLNK(st.st_mode))
      de->kind = BT_DIRENT_LINK;
    else if (S_ISDIR(st.st_mode))
      de->kind = BT_DIRENT_DIR;
    else if (S_ISREG(st.st_mode))
      de->kind = BT_DIRENT_FILE;
    else
      de->kind = BT_DIRENT_OTHER;
    g_ptr_array_add(out, de);
  }
  closedir(d);
  return out;
}

gchar *bt_platform_realpath(const gchar *path, GError **error)
{
  char *r = realpath(path, NULL);
  if (r == NULL) {
    set_errno_error(error, "Cannot resolve path", path, errno);
    return NULL;
  }
  gchar *out = g_strdup(r);
  free(r);
  return out;
}

gboolean bt_platform_replace_file(const gchar *src, const gchar *dst, GError **error)
{
  if (rename(src, dst) != 0) {
    set_errno_error(error, "Cannot replace", dst, errno);
    return FALSE;
  }
  return TRUE;
}

gboolean bt_platform_create_file_only(const gchar *src, const gchar *dst,
                                      gboolean *out_already_exists, GError **error)
{
  if (out_already_exists)
    *out_already_exists = FALSE;
  if (link(src, dst) != 0) {
    int err = errno;
    if (out_already_exists && err == EEXIST)
      *out_already_exists = TRUE;
    set_errno_error(error, "Cannot publish new file", dst, err);
    return FALSE;
  }
  if (unlink(src) != 0 && errno != ENOENT) {
    set_errno_error(error, "Cannot unlink temp file", src, errno);
    return FALSE;
  }
  return TRUE;
}

gboolean bt_platform_fsync_dir(const gchar *dir, GError **error)
{
  int fd = open(dir, O_RDONLY);
  if (fd < 0) {
    set_errno_error(error, "Cannot open directory for sync", dir, errno);
    return FALSE;
  }
  int rc = fsync(fd);
  int err = errno;
  close(fd);
  if (rc != 0 && err != EINVAL) { /* some filesystems refuse dir fsync */
    set_errno_error(error, "Cannot sync directory", dir, err);
    return FALSE;
  }
  return TRUE;
}
