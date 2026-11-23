/* createnote.c — Headless core note creation planner and name validator (Phase L3). */
#include "createnote.h"
#include "platform.h"

#include <glib/gstdio.h>
#include <string.h>

static gboolean is_ascii_hspace(char c)
{
  return c == ' ' || c == '\t';
}

static gboolean is_all_ws(const char *s)
{
  if (s == NULL)
    return TRUE;
  for (const char *p = s; *p != '\0'; p++) {
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      return FALSE;
  }
  return TRUE;
}

static gboolean contains_placeholder(const char *s)
{
  if (s == NULL)
    return FALSE;
  const char *open = strstr(s, "{{");
  while (open != NULL) {
    const char *p = open + 2;
    while (*p != '\0' && *p != '}' && *p != '\r' && *p != '\n')
      p++;
    if (p[0] == '}' && p[1] == '}')
      return TRUE;
    open = strstr(open + 2, "{{");
  }
  return FALSE;
}

static gchar *strip_note_ext_dup(const char *name)
{
  if (name == NULL)
    return NULL;
  gsize n = strlen(name);
  if (n > 3 && g_ascii_strcasecmp(name + n - 3, ".md") == 0)
    return g_strndup(name, n - 3);
  if (n > 9 && g_ascii_strcasecmp(name + n - 9, ".markdown") == 0)
    return g_strndup(name, n - 9);
  return g_strdup(name);
}

static gchar *path_dirname_rel(const char *rel)
{
  if (rel == NULL || rel[0] == '\0')
    return g_strdup("");
  const char *slash = strrchr(rel, '/');
  if (slash == NULL)
    return g_strdup("");
  return g_strndup(rel, (gsize)(slash - rel));
}

static gchar *lookup_actual_case_relpath(const BtVault *vault, const char *relpath)
{
  if (vault == NULL || relpath == NULL || relpath[0] == '\0')
    return g_strdup(relpath);
#ifdef G_OS_WIN32
  g_autofree gchar *abs = vault_resolve(vault, relpath, NULL);
  if (abs == NULL)
    return g_strdup(relpath);
  g_autofree gchar *real = bt_platform_realpath(abs, NULL);
  const char *root = vault_root(vault);
  if (real != NULL && root != NULL) {
    g_autofree gchar *real_fwd = g_strdup(real);
    g_autofree gchar *root_fwd = g_strdup(root);
    for (gchar *p = real_fwd; *p; p++)
      if (*p == '\\') *p = '/';
    for (gchar *p = root_fwd; *p; p++)
      if (*p == '\\') *p = '/';
    gsize rlen = strlen(root_fwd);
    while (rlen > 0 && root_fwd[rlen - 1] == '/') {
      root_fwd[rlen - 1] = '\0';
      rlen--;
    }
    if (strlen(real_fwd) > rlen + 1 &&
        g_ascii_strncasecmp(real_fwd, root_fwd, rlen) == 0 &&
        real_fwd[rlen] == '/') {
      return g_strdup(real_fwd + rlen + 1);
    }
  }
#endif
  return g_strdup(relpath);
}

gboolean bt_createnote_validate_name(const char *segment, gchar **out_reason)
{
  if (out_reason)
    *out_reason = NULL;
  if (segment == NULL || segment[0] == '\0' || is_all_ws(segment)) {
    if (out_reason)
      *out_reason = g_strdup("Empty name");
    return FALSE;
  }
  if (!g_utf8_validate(segment, -1, NULL)) {
    if (out_reason)
      *out_reason = g_strdup("Invalid UTF-8");
    return FALSE;
  }
  gsize slen = strlen(segment);
  if (slen > BT_CREATENOTE_MAX_SEGMENT_BYTES) {
    if (out_reason)
      *out_reason = g_strdup("Name too long");
    return FALSE;
  }
  for (gsize i = 0; i < slen; i++) {
    guchar uc = (guchar)segment[i];
    if (uc < 0x20u || uc == 0x7Fu) {
      if (out_reason)
        *out_reason = g_strdup("Contains a character Windows does not allow: control character");
      return FALSE;
    }
    if (uc == '<' || uc == '>' || uc == ':' || uc == '"' ||
        uc == '|' || uc == '?' || uc == '*' || uc == '/' || uc == '\\') {
      if (out_reason)
        *out_reason = g_strdup_printf("Contains a character Windows does not allow: %c", (char)uc);
      return FALSE;
    }
  }
  if (segment[slen - 1] == '.' || segment[slen - 1] == ' ' || segment[slen - 1] == '\t') {
    if (out_reason)
      *out_reason = g_strdup("Name ends with a dot or space");
    return FALSE;
  }
  if (bt_link_is_reserved_device_name(segment)) {
    if (out_reason)
      *out_reason = g_strdup("Reserved Windows name");
    return FALSE;
  }
  return TRUE;
}

static BtCreatePlan *make_plan_error(BtCreatePlanResult res, const char *reason)
{
  BtCreatePlan *p = g_new0(BtCreatePlan, 1);
  p->result = res;
  p->reason = g_strdup(reason);
  p->missing_folders = g_ptr_array_new_with_free_func(g_free);
  return p;
}

BtCreatePlan *bt_createnote_plan_with_syntax(const BtVault *vault, const BtIndexSnapshot *snap,
                                             const char *source_note, const char *raw_target,
                                             BtLinkSyntax syntax)
{
  if (raw_target == NULL)
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Empty name");

  /* Trim optional surrounding [[...]] and outer CR/LF (CR33), preserving trailing spaces for CR18. */
  gsize raw_len = strlen(raw_target);
  const char *s0 = raw_target;
  while (*s0 == '\r' || *s0 == '\n')
    s0++;
  raw_len = strlen(s0);
  while (raw_len > 0 && (s0[raw_len - 1] == '\r' || s0[raw_len - 1] == '\n'))
    raw_len--;

  g_autofree gchar *raw = NULL;
  if (raw_len >= 4 && s0[0] == '[' && s0[1] == '[' && s0[raw_len - 2] == ']' && s0[raw_len - 1] == ']')
    raw = g_strndup(s0 + 2, raw_len - 4);
  else
    raw = g_strndup(s0, raw_len);

  /* Split optional alias on | or \| before path validation (CR22, CR29). */
  g_autofree gchar *target_part = NULL;
  g_autofree gchar *alias_part = NULL;
  const char *pipe = strchr(raw, '|');
  if (pipe != NULL) {
    gsize tlen = (gsize)(pipe - raw);
    if (tlen > 0 && raw[tlen - 1] == '\\')
      tlen--;
    target_part = g_strndup(raw, tlen);
    const char *ap = pipe + 1;
    while (is_ascii_hspace(*ap))
      ap++;
    if (*ap != '\0') {
      alias_part = g_strdup(ap);
      g_strchomp(alias_part);
    }
  } else {
    target_part = g_strdup(raw);
  }

  /* Rule 1: Empty or whitespace-only target. */
  if (is_all_ws(target_part))
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Empty name");

  /* Trim leading horizontal space while preserving trailing space on filename (CR18). */
  const char *lead = target_part;
  while (is_ascii_hspace(*lead))
    lead++;

  /* Rule 2: Placeholder {{...}} gets no popover and no error (CR25). */
  if (contains_placeholder(lead))
    return make_plan_error(BT_CREATE_PLAN_PLACEHOLDER, NULL);

  /* Rule 3/9 pre-check: Windows drive-letter prefix (CR24) or UNC prefix is always outside vault. */
  if (g_str_has_prefix(lead, "\\\\") || g_str_has_prefix(lead, "//") ||
      (g_ascii_isalpha(lead[0]) && lead[1] == ':' &&
       (lead[2] == '\\' || lead[2] == '/' || lead[2] == '\0'))) {
    return make_plan_error(BT_CREATE_PLAN_OUTSIDE_VAULT, "Outside the vault");
  }

  /* Reject explicit single-slash rooted paths per §6.1.3 (CR23). */
  if (lead[0] == '/' || lead[0] == '\\')
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Absolute paths are not allowed");

  /* Split trailing #anchor and ^block_id (CR28). */
  g_autofree gchar *path_raw = g_strdup(lead);
  g_autofree gchar *anchor = NULL;
  g_autofree gchar *block_id = NULL;
  char *caret = strchr(path_raw, '^');
  char *hash = strchr(path_raw, '#');
  if (caret != NULL) {
    *caret = '\0';
    if (*(caret + 1) != '\0')
      block_id = g_strchomp(g_strdup(caret + 1));
  }
  if (hash != NULL) {
    *hash = '\0';
    if (*(hash + 1) != '\0')
      anchor = g_strchomp(g_strdup(hash + 1));
  }

  if (is_all_ws(path_raw))
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Empty name");

  /* Percent-decode Markdown targets (and keep matching L1). */
  g_autofree gchar *decoded = NULL;
  if (syntax == BT_LINK_SYNTAX_MARKDOWN || strchr(path_raw, '%') != NULL) {
    gboolean had_ctrl = FALSE;
    if (!bt_link_percent_decode_path(path_raw, &decoded, &had_ctrl))
      return make_plan_error(BT_CREATE_PLAN_INVALID_NAME,
                             "Contains a character Windows does not allow: control character");
  } else {
    decoded = g_strdup(path_raw);
  }

  /* Normalize '\' to '/' and collapse '//' (CR34, CR35). */
  GString *norm = g_string_sized_new(strlen(decoded));
  for (const char *p = decoded; *p != '\0'; p++) {
    char c = (*p == '\\') ? '/' : *p;
    if (c == '/' && norm->len > 0 && norm->str[norm->len - 1] == '/')
      continue;
    g_string_append_c(norm, c);
  }
  if (norm->len == 0) {
    g_string_free(norm, TRUE);
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Empty name");
  }
  if (norm->str[0] == '/') {
    g_string_free(norm, TRUE);
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Absolute paths are not allowed");
  }

  gboolean had_slash = (strchr(norm->str, '/') != NULL);
  gboolean trailing_slash = (norm->str[norm->len - 1] == '/');

  /* Lexical '..' and '.' resolution relative to source note directory (Rules 3 & 9, CR04, CR05). */
  g_autofree gchar *source_dir = path_dirname_rel(source_note);
  g_autoptr(GPtrArray) stack = g_ptr_array_new_with_free_func(g_free);
  if (source_dir[0] != '\0') {
    gchar **sd = g_strsplit(source_dir, "/", -1);
    for (guint i = 0; sd[i] != NULL; i++) {
      if (sd[i][0] != '\0')
        g_ptr_array_add(stack, g_strdup(sd[i]));
    }
    g_strfreev(sd);
  }

  gchar **raw_segs = g_strsplit(norm->str, "/", -1);
  g_string_free(norm, TRUE);
  for (guint i = 0; raw_segs[i] != NULL; i++) {
    const char *seg = raw_segs[i];
    if (seg[0] == '\0')
      continue;
    if (strcmp(seg, ".") == 0)
      continue;
    if (strcmp(seg, "..") == 0) {
      if (stack->len == 0) {
        g_strfreev(raw_segs);
        return make_plan_error(BT_CREATE_PLAN_OUTSIDE_VAULT, "Outside the vault");
      }
      g_ptr_array_set_size(stack, stack->len - 1);
      continue;
    }
    g_ptr_array_add(stack, g_strdup(seg));
  }
  g_strfreev(raw_segs);

  if (stack->len == 0)
    return make_plan_error(BT_CREATE_PLAN_OUTSIDE_VAULT, "Outside the vault");

  /* Rule 5: Trailing slash names a folder (CR11, CR30). Still validate segments first for safety. */
  for (guint i = 0; i < stack->len; i++) {
    const char *seg = g_ptr_array_index(stack, i);
    g_autofree gchar *vreason = NULL;
    if (!bt_createnote_validate_name(seg, &vreason))
      return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, vreason);
  }

  if (trailing_slash)
    return make_plan_error(BT_CREATE_PLAN_NOT_A_NOTE, "Folder");

  /* Rule 4: Non-note extension rejected unless extension absent or .md / .markdown (CR06..CR10). */
  const char *last_seg = g_ptr_array_index(stack, stack->len - 1);
  if (!bt_link_is_note_ext(last_seg)) {
    const char *dot = strrchr(last_seg, '.');
    if (dot != NULL && dot[1] != '\0') {
      g_autofree gchar *r = g_strdup_printf("Not a note (file type %s)", dot);
      return make_plan_error(BT_CREATE_PLAN_NOT_A_NOTE, r);
    }
  }

  /* Construct normalized vault-relative path with NFC normalization and .md fallback extension. */
  GString *joined = g_string_new(NULL);
  for (guint i = 0; i < stack->len; i++) {
    if (i > 0)
      g_string_append_c(joined, '/');
    g_string_append(joined, (const char *)g_ptr_array_index(stack, i));
  }
  if (!bt_link_is_note_ext(joined->str))
    g_string_append(joined, ".md");

  g_autofree gchar *rel_nfc = g_utf8_normalize(joined->str, -1, G_NORMALIZE_NFC);
  g_string_free(joined, TRUE);
  if (rel_nfc == NULL)
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Invalid UTF-8");
  if (strlen(rel_nfc) > BT_CREATENOTE_MAX_PATH_BYTES)
    return make_plan_error(BT_CREATE_PLAN_INVALID_NAME, "Name too long");

  /* Rule 3 / Symlink & path guard check through vault_resolve() (CR37). */
  g_autofree gchar *abs_target = NULL;
  if (vault != NULL) {
    g_autoptr(GError) verr = NULL;
    abs_target = vault_resolve(vault, rel_nfc, &verr);
    if (abs_target == NULL)
      return make_plan_error(BT_CREATE_PLAN_OUTSIDE_VAULT, "Outside the vault");
  }

  const char *base_ptr = strrchr(rel_nfc, '/');
  base_ptr = base_ptr ? (base_ptr + 1) : rel_nfc;
  g_autofree gchar *stem = strip_note_ext_dup(base_ptr);

  /* Rule 10: Snapshot exact-path existence check (CR13, CR14, CR15). */
  if (snap != NULL) {
    gboolean snap_is_dir = FALSE;
    g_autofree gchar *snap_actual = NULL;
    if (bt_index_snapshot_find_relpath(snap, rel_nfc, &snap_is_dir, &snap_actual)) {
      if (snap_is_dir)
        return make_plan_error(BT_CREATE_PLAN_NOT_A_NOTE, "Folder");
      BtCreatePlan *p = g_new0(BtCreatePlan, 1);
      p->result = BT_CREATE_PLAN_EXISTS;
      p->target_relpath = g_steal_pointer(&snap_actual);
      p->note_name = g_strdup(stem);
      p->display = g_strdup(alias_part ? alias_part : stem);
      p->anchor = g_steal_pointer(&anchor);
      p->block_id = g_steal_pointer(&block_id);
      p->missing_folders = g_ptr_array_new_with_free_func(g_free);
      return p;
    }
  }

  /* Rule 11: Disk existence check (catches stale index or files created outside snapshot). */
  if (abs_target != NULL && g_file_test(abs_target, G_FILE_TEST_EXISTS)) {
    if (g_file_test(abs_target, G_FILE_TEST_IS_DIR))
      return make_plan_error(BT_CREATE_PLAN_NOT_A_NOTE, "Folder");
    BtCreatePlan *p = g_new0(BtCreatePlan, 1);
    p->result = BT_CREATE_PLAN_EXISTS;
    p->target_relpath = lookup_actual_case_relpath(vault, rel_nfc);
    p->note_name = g_strdup(stem);
    p->display = g_strdup(alias_part ? alias_part : stem);
    p->anchor = g_steal_pointer(&anchor);
    p->block_id = g_steal_pointer(&block_id);
    p->missing_folders = g_ptr_array_new_with_free_func(g_free);
    p->stale_index = (snap != NULL);
    return p;
  }

  /* Rule 12: Plan OK. Identify any missing parent folders in creation order. */
  BtCreatePlan *p = g_new0(BtCreatePlan, 1);
  p->result = BT_CREATE_PLAN_OK;
  p->target_relpath = g_steal_pointer(&rel_nfc);
  p->note_name = g_strdup(stem);
  p->display = g_strdup(alias_part ? alias_part : stem);
  p->anchor = g_steal_pointer(&anchor);
  p->block_id = g_steal_pointer(&block_id);
  p->missing_folders = g_ptr_array_new_with_free_func(g_free);

  g_autofree gchar *parent_rel = path_dirname_rel(p->target_relpath);
  if (parent_rel[0] != '\0') {
    gchar **psegs = g_strsplit(parent_rel, "/", -1);
    GString *acc = g_string_new(NULL);
    for (guint i = 0; psegs[i] != NULL; i++) {
      if (psegs[i][0] == '\0')
        continue;
      if (acc->len > 0)
        g_string_append_c(acc, '/');
      g_string_append(acc, psegs[i]);
      gboolean missing = TRUE;
      if (vault != NULL) {
        g_autofree gchar *pabs = vault_resolve(vault, acc->str, NULL);
        if (pabs != NULL && g_file_test(pabs, G_FILE_TEST_IS_DIR))
          missing = FALSE;
      } else if (snap != NULL) {
        gboolean is_d = FALSE;
        if (bt_index_snapshot_find_relpath(snap, acc->str, &is_d, NULL) && is_d)
          missing = FALSE;
      }
      if (missing)
        g_ptr_array_add(p->missing_folders, g_strdup(acc->str));
    }
    g_string_free(acc, TRUE);
    g_strfreev(psegs);
  }

  p->needs_folder_create = (p->missing_folders->len > 0);
  p->has_folder_prefix = had_slash || p->needs_folder_create;
  if (snap != NULL)
    p->has_other_same_basename = bt_index_snapshot_has_other_basename(snap, stem, p->target_relpath);
  return p;
}

BtCreatePlan *bt_createnote_plan(const BtVault *vault, const BtIndexSnapshot *snap,
                                 const char *source_note, const char *raw_target)
{
  return bt_createnote_plan_with_syntax(vault, snap, source_note, raw_target,
                                        BT_LINK_SYNTAX_WIKILINK);
}

void bt_createnote_plan_free(BtCreatePlan *plan)
{
  if (plan == NULL)
    return;
  g_free(plan->target_relpath);
  g_free(plan->note_name);
  g_free(plan->display);
  g_free(plan->anchor);
  g_free(plan->block_id);
  g_free(plan->reason);
  if (plan->missing_folders != NULL)
    g_ptr_array_unref(plan->missing_folders);
  g_free(plan);
}

gchar *bt_createnote_plan_status_message(const BtCreatePlan *plan)
{
  if (plan == NULL)
    return NULL;
  switch (plan->result) {
  case BT_CREATE_PLAN_OK:
    return NULL;
  case BT_CREATE_PLAN_EXISTS:
    return plan->stale_index ? g_strdup("Index refreshed") : NULL;
  case BT_CREATE_PLAN_PLACEHOLDER:
    return NULL;
  case BT_CREATE_PLAN_NOT_A_NOTE:
    if (plan->reason != NULL && g_str_has_prefix(plan->reason, "Not a note:"))
      return g_strdup(plan->reason);
    return g_strdup_printf("Not a note: %s", plan->reason ? plan->reason : "invalid target");
  case BT_CREATE_PLAN_INVALID_NAME:
    return g_strdup_printf("Can't create: %s", plan->reason ? plan->reason : "Invalid name");
  case BT_CREATE_PLAN_OUTSIDE_VAULT:
    return g_strdup("Can't create: outside the vault");
  }
  return NULL;
}

const char *bt_createnote_plan_result_name(BtCreatePlanResult result)
{
  switch (result) {
  case BT_CREATE_PLAN_OK: return "PLAN_OK";
  case BT_CREATE_PLAN_EXISTS: return "PLAN_EXISTS";
  case BT_CREATE_PLAN_NOT_A_NOTE: return "PLAN_NOT_A_NOTE";
  case BT_CREATE_PLAN_INVALID_NAME: return "PLAN_INVALID_NAME";
  case BT_CREATE_PLAN_OUTSIDE_VAULT: return "PLAN_OUTSIDE_VAULT";
  case BT_CREATE_PLAN_PLACEHOLDER: return "PLAN_PLACEHOLDER";
  }
  return "UNKNOWN";
}

const char *bt_createnote_commit_result_name(BtCreateCommitResult result)
{
  switch (result) {
  case BT_CREATE_COMMIT_OK: return "COMMIT_OK";
  case BT_CREATE_COMMIT_EXISTS: return "COMMIT_EXISTS";
  case BT_CREATE_COMMIT_BLOCKED: return "COMMIT_BLOCKED";
  case BT_CREATE_COMMIT_IO_ERROR: return "COMMIT_IO_ERROR";
  }
  return "UNKNOWN";
}
