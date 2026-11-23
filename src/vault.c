/* vault.c — see vault.h. */
#ifndef G_OS_WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "vault.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#include <io.h>
#define bt_sys_write(fd, buf, n) _write((fd), (buf), (unsigned int)(n))
#else
#include <unistd.h>
#define bt_sys_write(fd, buf, n) write((fd), (buf), (n))
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define BT_SCAN_MAX_DEPTH 64
#define BT_WRITE_CHUNK (1u << 20)

struct BtVault {
  gchar *root;         /* canonical absolute root */
  GPtrArray *notes;    /* gchar* relpaths, sorted */
  GPtrArray *patterns; /* GPatternSpec* user ignore patterns */
  gboolean readonly;
};

static const gchar *const default_ignored_names[] = {
  ".git", ".obsidian", ".node_modules", "node_modules", "target", "dist", ".bt", NULL,
};

static gboolean name_equal(const gchar *a, const gchar *b)
{
#ifdef G_OS_WIN32
  return g_ascii_strcasecmp(a, b) == 0;
#else
  return strcmp(a, b) == 0;
#endif
}

static gboolean has_suffix_ci(const gchar *s, const gchar *suffix)
{
  gsize sl = strlen(s), xl = strlen(suffix);
  return sl > xl && g_ascii_strcasecmp(s + sl - xl, suffix) == 0;
}

static gboolean is_tmp_residue_name(const gchar *name)
{
  return has_suffix_ci(name, ".md.tmp") ||
         (g_str_has_prefix(name, ".bt-create-") && has_suffix_ci(name, ".tmp"));
}

static gboolean is_ignored_name(const BtVault *v, const gchar *name, const gchar *relpath)
{
  for (guint i = 0; default_ignored_names[i] != NULL; i++)
    if (name_equal(name, default_ignored_names[i]))
      return TRUE;
  if (is_tmp_residue_name(name))
    return TRUE;
  for (guint i = 0; i < v->patterns->len; i++) {
    GPatternSpec *p = g_ptr_array_index(v->patterns, i);
    if (g_pattern_spec_match_string(p, name) || (relpath && g_pattern_spec_match_string(p, relpath)))
      return TRUE;
  }
  return FALSE;
}

gboolean vault_is_ignored_path(const BtVault *vault, const gchar *relpath)
{
  g_return_val_if_fail(vault != NULL && relpath != NULL, TRUE);
  gchar **parts = g_strsplit_set(relpath, "/\\", -1);
  gboolean ignored = FALSE;
  for (guint i = 0; parts[i] != NULL && !ignored; i++)
    if (parts[i][0] != '\0' && is_ignored_name(vault, parts[i], relpath))
      ignored = TRUE;
  g_strfreev(parts);
  return ignored;
}

void vault_add_ignore_pattern(BtVault *vault, const gchar *glob)
{
  g_return_if_fail(vault != NULL && glob != NULL && *glob != '\0');
  g_ptr_array_add(vault->patterns, g_pattern_spec_new(glob));
}

/* ---------------------------------------------------------------- guard -- */

static gpointer reject(GError **error, const gchar *path, const gchar *why)
{
  g_set_error(error, BT_ERROR, BT_ERROR_PATH_REJECTED, "Path rejected: '%s' (%s)",
              path ? path : "(null)", why);
  return NULL;
}

/* TRUE iff path is strictly below root (never equal to it). */
static gboolean path_strictly_inside(const gchar *root, const gchar *path)
{
  gsize rl = strlen(root), pl = strlen(path);
  if (rl == 0 || pl <= rl)
    return FALSE;
  gboolean root_has_sep = G_IS_DIR_SEPARATOR(root[rl - 1]);
  if (!root_has_sep && !G_IS_DIR_SEPARATOR(path[rl]))
    return FALSE;
#ifdef G_OS_WIN32
  if (!g_utf8_validate(path, (gssize)rl, NULL))
    return FALSE;
  gchar *a = g_utf8_casefold(root, (gssize)rl);
  gchar *b = g_utf8_casefold(path, (gssize)rl);
  gboolean eq = strcmp(a, b) == 0;
  g_free(a);
  g_free(b);
  return eq;
#else
  return strncmp(root, path, rl) == 0;
#endif
}

#ifdef G_OS_WIN32
static gboolean is_reserved_device_component(const gchar *comp)
{
  static const gchar *const names[] = {
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
    "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL,
  };
  gsize stem = strcspn(comp, ".");
  gsize end = stem;
  while (end > 0 && comp[end - 1] == ' ')
    end--;
  for (guint i = 0; names[i] != NULL; i++)
    if (strlen(names[i]) == end && g_ascii_strncasecmp(comp, names[i], end) == 0)
      return TRUE;
  return FALSE;
}
#endif

gchar *vault_resolve(const BtVault *vault, const gchar *relpath, GError **error)
{
  g_return_val_if_fail(vault != NULL, NULL);
  if (relpath == NULL || relpath[0] == '\0')
    return reject(error, relpath, "empty path");
  if (!g_utf8_validate(relpath, -1, NULL))
    return reject(error, "<invalid UTF-8>", "not valid UTF-8");

  gchar *rel = g_strdup(relpath);
#ifdef G_OS_WIN32
  for (gchar *p = rel; *p != '\0'; p++)
    if (*p == '/')
      *p = '\\';
  if (strchr(rel, ':') != NULL) {
    g_free(rel);
    return reject(error, relpath, "drive letters and alternate data streams are not allowed");
  }
  {
    gchar **parts = g_strsplit(rel, "\\", -1);
    gboolean bad = FALSE;
    for (guint i = 0; parts[i] != NULL && !bad; i++)
      bad = is_reserved_device_component(parts[i]);
    g_strfreev(parts);
    if (bad) {
      g_free(rel);
      return reject(error, relpath, "reserved device name");
    }
  }
#endif
  if (g_path_is_absolute(rel) || G_IS_DIR_SEPARATOR(rel[0])) {
    g_free(rel);
    return reject(error, relpath, "absolute path");
  }

  gchar *joined = g_build_filename(vault->root, rel, NULL);
  g_free(rel);
  gchar *canon = g_canonicalize_filename(joined, NULL);
  g_free(joined);
  if (!path_strictly_inside(vault->root, canon)) {
    g_free(canon);
    return reject(error, relpath, "escapes the vault root");
  }

  /* Link escape: resolve the deepest existing ancestor (realpath semantics),
   * then re-append the not-yet-existing remainder, then re-check. */
  gchar *existing = g_strdup(canon);
  GStatBuf st;
  while (g_lstat(existing, &st) != 0) {
    gchar *parent = g_path_get_dirname(existing);
    if (strcmp(parent, existing) == 0) {
      g_free(parent);
      break;
    }
    g_free(existing);
    existing = parent;
  }
  gchar *real = bt_platform_realpath(existing, NULL);
  if (real == NULL) {
    g_free(existing);
    g_free(canon);
    return reject(error, relpath, "cannot be resolved (dangling link?)");
  }
  const gchar *rest = canon + strlen(existing);
  while (G_IS_DIR_SEPARATOR(*rest))
    rest++;
  gchar *final = *rest != '\0' ? g_build_filename(real, rest, NULL) : g_strdup(real);
  g_free(real);
  g_free(existing);
  g_free(canon);
  if (!path_strictly_inside(vault->root, final)) {
    g_free(final);
    return reject(error, relpath, "resolves outside the vault through a link");
  }
  return final;
}

/* ----------------------------------------------------------------- scan -- */

static void scan_dir(BtVault *v, const gchar *abs, const gchar *rel, guint depth, GPtrArray *notes,
                     BtScanStats *st)
{
  if (depth > BT_SCAN_MAX_DEPTH) {
    st->dirs_unreadable++;
    return;
  }
  GPtrArray *entries = bt_platform_list_dir(abs, NULL);
  if (entries == NULL) {
    st->dirs_unreadable++;
    return;
  }
  for (guint i = 0; i < entries->len; i++) {
    const BtDirent *d = g_ptr_array_index(entries, i);
    gchar *child_rel = rel[0] != '\0' ? g_strconcat(rel, "/", d->name, NULL) : g_strdup(d->name);

    if (is_tmp_residue_name(d->name) && d->kind != BT_DIRENT_DIR) {
      /* R1: residue of a crashed atomic write. The target is intact. */
      if (!v->readonly) {
        gchar *full = g_build_filename(abs, d->name, NULL);
        if (g_remove(full) == 0)
          st->tmp_removed++;
        g_free(full);
      }
    } else if (is_ignored_name(v, d->name, child_rel)) {
      /* skipped */
    } else if (d->kind == BT_DIRENT_DIR) {
      gchar *full = g_build_filename(abs, d->name, NULL);
      scan_dir(v, full, child_rel, depth + 1, notes, st);
      g_free(full);
    } else if (d->kind == BT_DIRENT_FILE && has_suffix_ci(d->name, ".md")) {
      g_ptr_array_add(notes, child_rel);
      child_rel = NULL;
    }
    g_free(child_rel);
  }
  g_ptr_array_unref(entries);
}

static gint cmp_relpath(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

gboolean vault_rescan(BtVault *vault, BtScanStats *stats, GError **error)
{
  g_return_val_if_fail(vault != NULL, FALSE);
  BtScanStats st = {0};
  gint64 t0 = g_get_monotonic_time();

  GPtrArray *probe = bt_platform_list_dir(vault->root, error);
  if (probe == NULL)
    return FALSE;
  g_ptr_array_unref(probe);

  GPtrArray *notes = g_ptr_array_new_with_free_func(g_free);
  scan_dir(vault, vault->root, "", 0, notes, &st);
  g_ptr_array_sort(notes, cmp_relpath);

  if (vault->notes != NULL)
    g_ptr_array_unref(vault->notes);
  vault->notes = notes;
  st.notes = notes->len;
  st.elapsed_us = g_get_monotonic_time() - t0;
  if (stats != NULL)
    *stats = st;
  return TRUE;
}

static BtVault *vault_open_internal(const gchar *path, gboolean readonly,
                                    BtScanStats *stats, GError **error)
{
  if (path == NULL || path[0] == '\0') {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "No vault path given");
    return NULL;
  }
  if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
    g_set_error(error, BT_ERROR, BT_ERROR_NOT_FOUND, "Vault not found: '%s'", path);
    return NULL;
  }
  gchar *real = bt_platform_realpath(path, error);
  if (real == NULL)
    return NULL;

  BtVault *v = g_new0(BtVault, 1);
  v->root = real;
  v->readonly = readonly;
  v->patterns = g_ptr_array_new_with_free_func((GDestroyNotify)g_pattern_spec_free);
  if (!vault_rescan(v, stats, error)) {
    vault_free(v);
    return NULL;
  }
  return v;
}

BtVault *vault_open(const gchar *path, BtScanStats *stats, GError **error)
{
  return vault_open_internal(path, FALSE, stats, error);
}

BtVault *vault_open_readonly(const gchar *path, BtScanStats *stats, GError **error)
{
  return vault_open_internal(path, TRUE, stats, error);
}

void vault_free(BtVault *vault)
{
  if (vault == NULL)
    return;
  g_free(vault->root);
  if (vault->notes != NULL)
    g_ptr_array_unref(vault->notes);
  g_ptr_array_unref(vault->patterns);
  g_free(vault);
}

const gchar *vault_root(const BtVault *vault)
{
  g_return_val_if_fail(vault != NULL, NULL);
  return vault->root;
}

const GPtrArray *vault_notes(const BtVault *vault)
{
  g_return_val_if_fail(vault != NULL, NULL);
  return vault->notes;
}

/* ------------------------------------------------------------------ io -- */

gboolean vault_read(const BtVault *vault, const gchar *relpath, gchar **contents, gsize *length,
                    GError **error)
{
  g_return_val_if_fail(contents != NULL, FALSE);
  gchar *abs = vault_resolve(vault, relpath, error);
  if (abs == NULL)
    return FALSE;
  GError *local = NULL;
  gboolean ok = g_file_get_contents(abs, contents, length, &local);
  if (!ok) {
    g_set_error(error, BT_ERROR,
                g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT) ? BT_ERROR_NOT_FOUND
                                                                         : BT_ERROR_IO,
                "Cannot read '%s': %s", relpath, local->message);
    g_error_free(local);
  }
  g_free(abs);
  return ok;
}

static void run_hook(const BtWriteHooks *hooks, BtWriteStage stage)
{
  if (hooks != NULL && hooks->at_stage != NULL)
    hooks->at_stage(stage, hooks->user_data);
}

static gboolean write_all(int fd, const gchar *data, gsize length, const gchar *path, GError **error)
{
  gsize done = 0;
  while (done < length) {
    gsize n = MIN(length - done, (gsize)BT_WRITE_CHUNK);
    int w = bt_sys_write(fd, data + done, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot write '%s': %s", path, g_strerror(errno));
      return FALSE;
    }
    done += (gsize)w;
  }
  return TRUE;
}

gboolean vault_atomic_write(const BtVault *vault, const gchar *relpath, const gchar *data,
                            gsize length, const BtWriteHooks *hooks, GError **error)
{
  g_return_val_if_fail(data != NULL || length == 0, FALSE);
  if (relpath != NULL && has_suffix_ci(relpath, ".tmp")) {
    reject(error, relpath, "temporary-file names are reserved");
    return FALSE;
  }
  gchar *abs = vault_resolve(vault, relpath, error);
  if (abs == NULL)
    return FALSE;

  gboolean ok = FALSE;
  gchar *dir = g_path_get_dirname(abs);
  gchar *tmp = g_strconcat(abs, ".tmp", NULL);
  int fd = -1;

  if (g_mkdir_with_parents(dir, 0755) != 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot create folder for '%s': %s", relpath,
                g_strerror(errno));
    goto out;
  }
  /* Exclusive create: never follow a planted link at the temp name (§8.5).
   * A stale temp from a crash is removed once and the create retried. */
  fd = g_open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
  if (fd < 0 && errno == EEXIST) {
    g_remove(tmp);
    fd = g_open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
  }
  if (fd < 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot create temp file for '%s': %s", relpath,
                g_strerror(errno));
    goto out;
  }
  run_hook(hooks, BT_WRITE_STAGE_TMP_OPENED);

  gsize half = length / 2;
  if (!write_all(fd, data, half, relpath, error))
    goto fail_tmp;
  run_hook(hooks, BT_WRITE_STAGE_TMP_PARTIAL);
  if (!write_all(fd, data + half, length - half, relpath, error))
    goto fail_tmp;
  if (g_fsync(fd) != 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot flush '%s': %s", relpath, g_strerror(errno));
    goto fail_tmp;
  }
  run_hook(hooks, BT_WRITE_STAGE_TMP_SYNCED);
  if (!g_close(fd, NULL)) {
    fd = -1;
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot close temp file for '%s'", relpath);
    goto fail_tmp;
  }
  fd = -1;

  run_hook(hooks, BT_WRITE_STAGE_BEFORE_RENAME);
  if (!bt_platform_replace_file(tmp, abs, error))
    goto fail_tmp;
  run_hook(hooks, BT_WRITE_STAGE_AFTER_RENAME);
  ok = bt_platform_fsync_dir(dir, error);
  goto out;

fail_tmp:
  if (fd >= 0)
    g_close(fd, NULL);
  fd = -1;
  g_remove(tmp);
out:
  g_free(tmp);
  g_free(dir);
  g_free(abs);
  return ok;
}

gboolean vault_rename_note(const BtVault *vault, const gchar *old_relpath,
                           const gchar *new_relpath, GError **error)
{
  g_return_val_if_fail(vault != NULL && old_relpath != NULL && new_relpath != NULL, FALSE);
  if (has_suffix_ci(new_relpath, ".tmp") || !has_suffix_ci(new_relpath, ".md")) {
    reject(error, new_relpath, "note filename must end with .md");
    return FALSE;
  }
  if (vault_is_ignored_path(vault, new_relpath)) {
    reject(error, new_relpath, "destination path is in an ignored folder");
    return FALSE;
  }

  gchar *old_abs = vault_resolve(vault, old_relpath, error);
  if (old_abs == NULL)
    return FALSE;
  gchar *new_abs = vault_resolve(vault, new_relpath, error);
  if (new_abs == NULL) {
    g_free(old_abs);
    return FALSE;
  }

  gboolean ok = FALSE;
  gchar *old_dir = g_path_get_dirname(old_abs);
  gchar *new_dir = g_path_get_dirname(new_abs);

  if (!g_file_test(old_abs, G_FILE_TEST_IS_REGULAR)) {
    g_set_error(error, BT_ERROR, BT_ERROR_NOT_FOUND, "Note not found: '%s'", old_relpath);
    goto out;
  }
  if (strcmp(old_abs, new_abs) == 0) {
    ok = TRUE;
    goto out;
  }

  gboolean case_only = (g_ascii_strcasecmp(old_abs, new_abs) == 0);
  if (!case_only && g_file_test(new_abs, G_FILE_TEST_EXISTS)) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "A file named '%s' already exists", new_relpath);
    goto out;
  }
  if (g_mkdir_with_parents(new_dir, 0755) != 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot create folder for '%s': %s",
                new_relpath, g_strerror(errno));
    goto out;
  }

  if (case_only) {
    /* Two-step rename ensures case-only changes commit cleanly on NTFS/Windows. */
    gchar *hop = g_strconcat(old_abs, ".bt-rename-hop.tmp", NULL);
    if (g_rename(old_abs, hop) != 0) {
      g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot rename '%s': %s",
                  old_relpath, g_strerror(errno));
      g_free(hop);
      goto out;
    }
    if (g_rename(hop, new_abs) != 0) {
      g_rename(hop, old_abs);
      g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot rename '%s' to '%s': %s",
                  old_relpath, new_relpath, g_strerror(errno));
      g_free(hop);
      goto out;
    }
    g_free(hop);
  } else {
    if (g_rename(old_abs, new_abs) != 0) {
      g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot rename '%s' to '%s': %s",
                  old_relpath, new_relpath, g_strerror(errno));
      goto out;
    }
  }

  bt_platform_fsync_dir(old_dir, NULL);
  if (strcmp(old_dir, new_dir) != 0)
    bt_platform_fsync_dir(new_dir, NULL);
  ok = TRUE;

out:
  g_free(new_dir);
  g_free(old_dir);
  g_free(new_abs);
  g_free(old_abs);
  return ok;
}

gboolean vault_delete_note(const BtVault *vault, const gchar *relpath, GError **error)
{
  g_return_val_if_fail(vault != NULL && relpath != NULL, FALSE);
  gchar *abs = vault_resolve(vault, relpath, error);
  if (abs == NULL)
    return FALSE;

  gboolean ok = FALSE;
  if (g_file_test(abs, G_FILE_TEST_IS_DIR)) {
    g_set_error(error, BT_ERROR, BT_ERROR_INVALID, "Cannot delete directory: '%s'", relpath);
    g_free(abs);
    return FALSE;
  }
  if (!g_file_test(abs, G_FILE_TEST_IS_REGULAR)) {
    g_set_error(error, BT_ERROR, BT_ERROR_NOT_FOUND, "File not found: '%s'", relpath);
    g_free(abs);
    return FALSE;
  }
  if (g_remove(abs) != 0) {
    g_set_error(error, BT_ERROR, BT_ERROR_IO, "Cannot delete '%s': %s", relpath, g_strerror(errno));
    g_free(abs);
    return FALSE;
  }
  gchar *dir = g_path_get_dirname(abs);
  bt_platform_fsync_dir(dir, NULL);
  g_free(dir);
  g_free(abs);
  ok = TRUE;
  return ok;
}

void bt_vault_log(const BtVault *vault, const gchar *level, const gchar *fmt, ...)
{
  if (level == NULL || fmt == NULL)
    return;
  va_list ap;
  va_start(ap, fmt);
  g_autofree gchar *raw_msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);

  /* Enforce single-line rule (§16): replace embedded CR/LF with spaces. */
  for (gchar *p = raw_msg; *p != '\0'; p++) {
    if (*p == '\r' || *p == '\n')
      *p = ' ';
  }

  g_autoptr(GDateTime) now = g_date_time_new_now_utc();
  g_autofree gchar *ts = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
  g_autofree gchar *line = g_strdup_printf("[%s] %s: %s", ts ? ts : "unknown", level, raw_msg);

  if (g_ascii_strcasecmp(level, "WARNING") == 0 || g_ascii_strcasecmp(level, "ERROR") == 0)
    g_warning("%s", line);

  if (vault != NULL && !vault->readonly && vault->root != NULL) {
    static GMutex log_mu;
    g_mutex_lock(&log_mu);
    g_autofree gchar *bt_dir = g_build_filename(vault->root, ".bt", NULL);
    g_mkdir_with_parents(bt_dir, 0755);
    g_autofree gchar *log_path = g_build_filename(bt_dir, "vault.log", NULL);
    FILE *fp = g_fopen(log_path, "ab");
    if (fp != NULL) {
      fprintf(fp, "%s\n", line);
      fclose(fp);
    }
    g_mutex_unlock(&log_mu);
  }
}
