/* platform_win.c — Win32 implementations of platform.h. */
#include "platform.h"
#include "bterror.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>

void bt_dirent_free(gpointer dirent)
{
  BtDirent *d = dirent;
  if (d == NULL)
    return;
  g_free(d->name);
  g_free(d);
}

static void set_win32_error(GError **error, const gchar *what, const gchar *path, DWORD code)
{
  gchar *msg = g_win32_error_message((gint)code);
  g_set_error(error, BT_ERROR, code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                                   ? BT_ERROR_NOT_FOUND : BT_ERROR_IO,
              "%s '%s': %s", what, path, msg);
  g_free(msg);
}

static wchar_t *to_wide(const gchar *path, GError **error)
{
  wchar_t *w = (wchar_t *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
  if (w == NULL)
    g_set_error(error, BT_ERROR, BT_ERROR_ENCODING, "Path is not valid UTF-8: '%s'", path);
  return w;
}

/* Only true links are treated as links; cloud placeholders (OneDrive) and
 * other reparse tags are regular files/dirs for scanning purposes. */
static gboolean is_link_tag(DWORD tag)
{
  return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

GPtrArray *bt_platform_list_dir(const gchar *dir, GError **error)
{
  gchar *pattern = g_build_filename(dir, "*", NULL);
  wchar_t *wpat = to_wide(pattern, error);
  g_free(pattern);
  if (wpat == NULL)
    return NULL;

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW(wpat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                              FIND_FIRST_EX_LARGE_FETCH);
  g_free(wpat);
  GPtrArray *out = g_ptr_array_new_with_free_func(bt_dirent_free);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND)
      return out;
    g_ptr_array_unref(out);
    set_win32_error(error, "Cannot list directory", dir, code);
    return NULL;
  }

  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
      continue;
    gchar *name = g_utf16_to_utf8((const gunichar2 *)fd.cFileName, -1, NULL, NULL, NULL);
    if (name == NULL)
      continue; /* unpaired surrogates: cannot be represented, skip */
    BtDirent *d = g_new0(BtDirent, 1);
    d->name = name;
    DWORD attr = fd.dwFileAttributes;
    if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(fd.dwReserved0))
      d->kind = BT_DIRENT_LINK;
    else if (attr & FILE_ATTRIBUTE_DIRECTORY)
      d->kind = BT_DIRENT_DIR;
    else if (attr & FILE_ATTRIBUTE_DEVICE)
      d->kind = BT_DIRENT_OTHER;
    else
      d->kind = BT_DIRENT_FILE;
    g_ptr_array_add(out, d);
  } while (FindNextFileW(h, &fd));

  DWORD code = GetLastError();
  FindClose(h);
  if (code != ERROR_NO_MORE_FILES) {
    g_ptr_array_unref(out);
    set_win32_error(error, "Error while listing directory", dir, code);
    return NULL;
  }
  return out;
}

gchar *bt_platform_realpath(const gchar *path, GError **error)
{
  wchar_t *wpath = to_wide(path, error);
  if (wpath == NULL)
    return NULL;
  HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  g_free(wpath);
  if (h == INVALID_HANDLE_VALUE) {
    set_win32_error(error, "Cannot resolve path", path, GetLastError());
    return NULL;
  }
  DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
  DWORD need = GetFinalPathNameByHandleW(h, NULL, 0, flags);
  if (need == 0) {
    set_win32_error(error, "Cannot resolve path", path, GetLastError());
    CloseHandle(h);
    return NULL;
  }
  wchar_t *buf = g_new(wchar_t, (gsize)need + 1);
  DWORD got = GetFinalPathNameByHandleW(h, buf, need + 1, flags);
  DWORD code = GetLastError();
  CloseHandle(h);
  if (got == 0 || got > need) {
    g_free(buf);
    set_win32_error(error, "Cannot resolve path", path, code);
    return NULL;
  }

  const wchar_t *p = buf;
  gboolean unc = FALSE;
  if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
    p += 8;
    unc = TRUE;
  } else if (wcsncmp(p, L"\\\\?\\", 4) == 0) {
    p += 4;
  }
  gchar *utf8 = g_utf16_to_utf8((const gunichar2 *)p, -1, NULL, NULL, NULL);
  g_free(buf);
  if (utf8 == NULL) {
    g_set_error(error, BT_ERROR, BT_ERROR_ENCODING, "Resolved path of '%s' is not representable", path);
    return NULL;
  }
  if (unc) {
    gchar *tmp = g_strconcat("\\\\", utf8, NULL);
    g_free(utf8);
    utf8 = tmp;
  }
  return utf8;
}

gboolean bt_platform_replace_file(const gchar *src, const gchar *dst, GError **error)
{
  wchar_t *wsrc = to_wide(src, error);
  if (wsrc == NULL)
    return FALSE;
  wchar_t *wdst = to_wide(dst, error);
  if (wdst == NULL) {
    g_free(wsrc);
    return FALSE;
  }
  BOOL ok = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  DWORD code = GetLastError();
  g_free(wsrc);
  g_free(wdst);
  if (!ok) {
    set_win32_error(error, "Cannot replace", dst, code);
    return FALSE;
  }
  return TRUE;
}

gboolean bt_platform_fsync_dir(const gchar *dir, GError **error)
{
  (void)dir;
  (void)error;
  return TRUE;
}
