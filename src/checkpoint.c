/* checkpoint.c — libgit2 checkpoint/rollback façade (ADR-006, P7, R10). */
#include "checkpoint.h"

#include <git2.h>
#include <string.h>

#define BT_GIT_AUTHOR_NAME  "brainterm-agent"
#define BT_GIT_AUTHOR_EMAIL "noreply@brainterm.local"

void bt_checkpoint_info_free(gpointer info)
{
  BtCheckpointInfo *ci = info;
  if (ci == NULL)
    return;
  g_free(ci->message);
  g_free(ci);
}

void bt_checkpoint_init(void)
{
  git_libgit2_init();
}

void bt_checkpoint_shutdown(void)
{
  git_libgit2_shutdown();
}

static void set_git_err(GError **error, const gchar *what, int rc)
{
  const git_error *e = git_error_last();
  g_set_error(error, BT_ERROR, BT_ERROR_IO, "%s: %s (code %d)", what,
              (e && e->message) ? e->message : "libgit2 error", rc);
}

static git_repository *open_or_init_repo(const gchar *vault_root, gboolean create_if_missing,
                                         GError **error)
{
  git_repository *repo = NULL;
  int rc = git_repository_open(&repo, vault_root);
  if (rc == 0)
    return repo;
  if (!create_if_missing) {
    set_git_err(error, "Cannot open git repository", rc);
    return NULL;
  }
  git_repository_init_options opts;
  git_repository_init_options_init(&opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
  opts.flags = GIT_REPOSITORY_INIT_MKPATH;
  opts.initial_head = "main";
  rc = git_repository_init_ext(&repo, vault_root, &opts);
  if (rc != 0) {
    set_git_err(error, "Cannot initialize git repository", rc);
    return NULL;
  }
  git_config *cfg = NULL;
  if (git_repository_config(&cfg, repo) == 0) {
    git_config_set_bool(cfg, "core.autocrlf", 0);
    git_config_free(cfg);
  }
  return repo;
}

gboolean bt_checkpoint_take(const gchar *vault_root, const gchar *message,
                            gchar oid_out[BT_OID_HEX_LEN], GError **error)
{
  g_return_val_if_fail(vault_root != NULL && message != NULL, FALSE);

  /* Ensure .bt/ and *.md.tmp are ignored if no .gitignore exists yet. */
  gchar *gi_path = g_build_filename(vault_root, ".gitignore", NULL);
  if (!g_file_test(gi_path, G_FILE_TEST_EXISTS))
    g_file_set_contents(gi_path, ".bt/\n*.md.tmp\n", -1, NULL);
  g_free(gi_path);

  git_repository *repo = open_or_init_repo(vault_root, TRUE, error);
  if (repo == NULL)
    return FALSE;

  gboolean ok = FALSE;
  git_index *index = NULL;
  git_tree *tree = NULL;
  git_signature *sig = NULL;
  git_commit *parent = NULL;
  git_object *head_obj = NULL;

  int rc = git_repository_index(&index, repo);
  if (rc != 0) {
    set_git_err(error, "Cannot open git index", rc);
    goto out;
  }

  /* Stage all additions, modifications, and deletions in the working tree. */
  rc = git_index_add_all(index, NULL, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
  if (rc != 0) {
    set_git_err(error, "Cannot stage vault files", rc);
    goto out;
  }
  rc = git_index_update_all(index, NULL, NULL, NULL);
  if (rc != 0) {
    set_git_err(error, "Cannot update git index", rc);
    goto out;
  }
  rc = git_index_write(index);
  if (rc != 0) {
    set_git_err(error, "Cannot write git index (repository locked?)", rc);
    goto out;
  }

  git_oid tree_oid;
  rc = git_index_write_tree(&tree_oid, index);
  if (rc != 0) {
    set_git_err(error, "Cannot write git tree", rc);
    goto out;
  }
  rc = git_tree_lookup(&tree, repo, &tree_oid);
  if (rc != 0) {
    set_git_err(error, "Cannot lookup git tree", rc);
    goto out;
  }

  rc = git_signature_now(&sig, BT_GIT_AUTHOR_NAME, BT_GIT_AUTHOR_EMAIL);
  if (rc != 0) {
    set_git_err(error, "Cannot create git signature", rc);
    goto out;
  }

  const git_commit *parents[1] = {NULL};
  size_t parent_count = 0;
  if (git_revparse_single(&head_obj, repo, "HEAD") == 0 &&
      git_object_type(head_obj) == GIT_OBJECT_COMMIT) {
    parent = (git_commit *)head_obj;
    head_obj = NULL;
    parents[0] = parent;
    parent_count = 1;
  }

  git_oid commit_oid;
  rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig, "UTF-8", message, tree, parent_count,
                         parents);
  if (rc != 0) {
    set_git_err(error, "Cannot create checkpoint commit", rc);
    goto out;
  }

  if (oid_out != NULL)
    git_oid_tostr(oid_out, BT_OID_HEX_LEN, &commit_oid);
  ok = TRUE;

out:
  git_object_free(head_obj);
  git_commit_free(parent);
  git_signature_free(sig);
  git_tree_free(tree);
  git_index_free(index);
  git_repository_free(repo);
  return ok;
}

gboolean bt_checkpoint_rollback(const gchar *vault_root, const gchar *ref_or_oid, GError **error)
{
  g_return_val_if_fail(vault_root != NULL && ref_or_oid != NULL, FALSE);
  git_repository *repo = open_or_init_repo(vault_root, FALSE, error);
  if (repo == NULL)
    return FALSE;

  git_object *target = NULL;
  int rc = git_revparse_single(&target, repo, ref_or_oid);
  if (rc != 0) {
    set_git_err(error, "Cannot resolve checkpoint ref", rc);
    git_repository_free(repo);
    return FALSE;
  }

  git_checkout_options co;
  git_checkout_options_init(&co, GIT_CHECKOUT_OPTIONS_VERSION);
  co.checkout_strategy = GIT_CHECKOUT_FORCE;
  rc = git_reset(repo, target, GIT_RESET_HARD, &co);
  git_object_free(target);
  git_repository_free(repo);
  if (rc != 0) {
    set_git_err(error, "Rollback failed", rc);
    return FALSE;
  }
  return TRUE;
}

GPtrArray *bt_checkpoint_list(const gchar *vault_root, guint max_count, GError **error)
{
  g_return_val_if_fail(vault_root != NULL, NULL);
  git_repository *repo = open_or_init_repo(vault_root, FALSE, error);
  if (repo == NULL)
    return NULL;

  GPtrArray *out = g_ptr_array_new_with_free_func(bt_checkpoint_info_free);
  git_revwalk *walk = NULL;
  if (git_revwalk_new(&walk, repo) == 0 && git_revwalk_push_head(walk) == 0) {
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    git_oid oid;
    guint count = 0;
    while ((max_count == 0 || count < max_count) && git_revwalk_next(&oid, walk) == 0) {
      git_commit *c = NULL;
      if (git_commit_lookup(&c, repo, &oid) == 0) {
        BtCheckpointInfo *ci = g_new0(BtCheckpointInfo, 1);
        git_oid_tostr(ci->oid, BT_OID_HEX_LEN, &oid);
        const char *msg = git_commit_summary(c);
        ci->message = g_strdup(msg ? msg : "");
        ci->timestamp = (gint64)git_commit_time(c);
        g_ptr_array_add(out, ci);
        git_commit_free(c);
        count++;
      }
    }
  }
  git_revwalk_free(walk);
  git_repository_free(repo);
  return out;
}

gboolean bt_checkpoint_read_file(const gchar *vault_root, const gchar *ref_or_oid,
                                 const gchar *relpath, gchar **contents, gsize *length,
                                 GError **error)
{
  g_return_val_if_fail(vault_root != NULL && ref_or_oid != NULL && relpath != NULL &&
                           contents != NULL,
                       FALSE);
  git_repository *repo = open_or_init_repo(vault_root, FALSE, error);
  if (repo == NULL)
    return FALSE;

  gboolean ok = FALSE;
  git_object *obj = NULL;
  git_commit *commit = NULL;
  git_tree *tree = NULL;
  git_tree_entry *entry = NULL;
  git_blob *blob = NULL;

  int rc = git_revparse_single(&obj, repo, ref_or_oid);
  if (rc != 0 || git_object_peel((git_object **)&commit, obj, GIT_OBJECT_COMMIT) != 0) {
    set_git_err(error, "Cannot resolve checkpoint commit", rc);
    goto out;
  }
  rc = git_commit_tree(&tree, commit);
  if (rc != 0) {
    set_git_err(error, "Cannot read checkpoint tree", rc);
    goto out;
  }
  rc = git_tree_entry_bypath(&entry, tree, relpath);
  if (rc != 0) {
    set_git_err(error, "File not found in checkpoint", rc);
    goto out;
  }
  rc = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
  if (rc != 0) {
    set_git_err(error, "Cannot read blob from checkpoint", rc);
    goto out;
  }
  gsize sz = (gsize)git_blob_rawsize(blob);
  const void *raw = git_blob_rawcontent(blob);
  gchar *buf = g_malloc(sz + 1);
  if (sz > 0)
    memcpy(buf, raw, sz);
  buf[sz] = '\0';
  *contents = buf;
  if (length != NULL)
    *length = sz;
  ok = TRUE;

out:
  git_blob_free(blob);
  git_tree_entry_free(entry);
  git_tree_free(tree);
  git_commit_free(commit);
  git_object_free(obj);
  git_repository_free(repo);
  return ok;
}
