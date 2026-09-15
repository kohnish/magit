#include "kstring.h"
#include "msgpack_handler.h"
#include "util.h"
#include <git2.h>
#
typedef struct {
    uv_work_t req;
    uint64_t id;
    kstring_t *pwd;
    kstring_t *root;
    kstring_t *rev_head;
    kstring_t *list_z;
    kstring_t *branch;
    kstring_t *upstream_branch;
    kstring_t *head_log_line;
    kstring_t *subj;
    kstring_t *upstream_subj;
    kstring_t *tag_desc;
    kstring_t *opt_tag_desc_head;
    kstring_t *version;
    kstring_t *worktree_porcelain;
    int result;
} git_root_req_T;

git_repository *g_repo = NULL;
kstring_t *g_dot_git_dir = NULL;

static void git_root_request_cleanup(git_root_req_T **req) {
    if (req && *req) {
        if ((*req)->pwd) {
            free((*req)->pwd->s);
            free((*req)->pwd);
        }
        if ((*req)->root) {
            free((*req)->root->s);
            free((*req)->root);
        }
        if ((*req)->rev_head) {
            free((*req)->rev_head->s);
            free((*req)->rev_head);
        }
        if ((*req)->list_z) {
            free((*req)->list_z->s);
            free((*req)->list_z);
        }
        if ((*req)->branch) {
            free((*req)->branch->s);
            free((*req)->branch);
        }
        if ((*req)->upstream_subj) {
            free((*req)->upstream_subj->s);
            free((*req)->upstream_subj);
        }
        if ((*req)->version) {
            free((*req)->version->s);
            free((*req)->version);
        }
        if ((*req)->subj) {
            free((*req)->subj->s);
            free((*req)->subj);
        }
        if ((*req)->opt_tag_desc_head) {
            free((*req)->opt_tag_desc_head->s);
            free((*req)->opt_tag_desc_head);
        }
        if ((*req)->worktree_porcelain) {
            free((*req)->worktree_porcelain->s);
            free((*req)->worktree_porcelain);
        }
        free(*req);
    }
};

#define GIT_ROOT_REQUEST_CLEANUP __attribute__((cleanup(git_root_request_cleanup)))

typedef struct {
    git_repository *repo;
    const git_oid *target;
    char *match;
} find_tag_ctx;

static int list_worktrees_porcelain(git_repository *repo, kstring_t *out) {
    git_reference *head_ref = NULL;
    git_strarray wt_names = {0};
    int is_bare = git_repository_is_bare(repo);
    int ret;
    /* ---- main worktree ---- */
    {
        const char *main_path = is_bare ? git_repository_path(repo) : git_repository_workdir(repo);

        kputs("worktree ", out);
        kputs(main_path, out);
        kputc(0, out);

        if (git_repository_head(&head_ref, repo) == 0) {
            char oidstr[GIT_OID_HEXSZ + 1];
            git_oid_tostr(oidstr, sizeof(oidstr), git_reference_target(head_ref));

            kputs("HEAD ", out);
            kputs(oidstr, out);
            kputc(0, out);

            if (git_repository_head_detached(repo) == 1) {
                kputs("detached", out);
                kputc(0, out);
            } else {
                kputs("branch ", out);
                kputs(git_reference_name(head_ref), out);
                kputc(0, out);
            }
            git_reference_free(head_ref);
        }
        /* unborn HEAD case skipped here; real git prints an all-zero oid line */

        if (is_bare) {
            kputs("bare", out);
            kputc(0, out);
        }

        kputc(0, out); /* entry terminator */
    }

    /* ---- linked worktrees ---- */
    ret = git_worktree_list(&wt_names, repo);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < wt_names.count; i++) {
        git_worktree *wt = NULL;
        git_repository *wt_repo = NULL;
        git_buf lock_reason = GIT_BUF_INIT;

        if (git_worktree_lookup(&wt, repo, wt_names.strings[i]) < 0)
            continue;

        kputs("worktree ", out);
        kputs(git_worktree_path(wt), out);
        kputc(0, out);

        if (git_repository_open_from_worktree(&wt_repo, wt) == 0) {
            if (git_repository_head(&head_ref, wt_repo) == 0) {
                char oidstr[GIT_OID_HEXSZ + 1];
                git_oid_tostr(oidstr, sizeof(oidstr), git_reference_target(head_ref));

                kputs("HEAD ", out);
                kputs(oidstr, out);
                kputc(0, out);

                if (git_repository_head_detached(wt_repo) == 1) {
                    kputs("detached", out);
                    kputc(0, out);
                } else {
                    kputs("branch ", out);
                    kputs(git_reference_name(head_ref), out);
                    kputc(0, out);
                }
                git_reference_free(head_ref);
            }
            git_repository_free(wt_repo);
        }

        if (git_worktree_is_locked(&lock_reason, wt) > 0) {
            kputs("locked", out);
            if (lock_reason.ptr && *lock_reason.ptr) {
                kputc(' ', out);
                kputs(lock_reason.ptr, out);
            }
            kputc(0, out);
        }
        git_buf_dispose(&lock_reason);

        if (git_worktree_is_prunable(wt, NULL) > 0) {
            kputs("prunable", out);
            kputc(0, out);
        }

        kputc(0, out); /* entry terminator */
        git_worktree_free(wt);
    }

    git_strarray_free(&wt_names);
    return 0;
}

static int tag_cb(const char *name, git_oid *oid, void *payload)
{
    find_tag_ctx *ctx = payload;
    git_object *obj = NULL, *commit = NULL;

    if (git_object_lookup(&obj, ctx->repo, oid, GIT_OBJECT_ANY) < 0)
        return 0;

    if (git_object_peel(&commit, obj, GIT_OBJECT_COMMIT) == 0 &&
        git_oid_equal(git_object_id(commit), ctx->target)) {
        const char *n = name;
        if (strncmp(n, "refs/tags/", 10) == 0)
            n += 10;
        ctx->match = strdup(n);
        git_object_free(commit);
        git_object_free(obj);
        return 1; /* nonzero stops git_tag_foreach */
    }

    if (commit) git_object_free(commit);
    git_object_free(obj);
    return 0;
}

static int get_tag_desc_if_match(git_repository *repo, kstring_t *out) {
    git_object *head = NULL;
    find_tag_ctx ctx = {.repo = repo};
    int ret;

    ret = git_revparse_single(&head, repo, "HEAD");
    if (ret < 0)
        return ret;

    ctx.target = git_object_id(head);

    ret = git_tag_foreach(repo, tag_cb, &ctx);
    git_object_free(head);
    if (ret < 0 && ret != GIT_ENOTFOUND)
        return ret;

    if (ctx.match) {
        kputs(ctx.match, out);
        free(ctx.match);
    }

    return 0;
}

static int get_tag_desc(git_repository *repo, kstring_t *out) {
    git_object *head = NULL;
    git_describe_result *result = NULL;
    git_describe_options opts = GIT_DESCRIBE_OPTIONS_INIT;
    git_describe_format_options fmt = GIT_DESCRIBE_FORMAT_OPTIONS_INIT;
    git_buf buf = GIT_BUF_INIT;
    int ret;

    ret = git_revparse_single(&head, repo, "HEAD");
    if (ret < 0)
        return ret;

    opts.describe_strategy = GIT_DESCRIBE_TAGS;

    ret = git_describe_commit(&result, head, &opts);
    git_object_free(head);

    if (ret < 0)
        return ret;

    fmt.always_use_long_format = 1;

    ret = git_describe_format(&buf, result, &fmt);
    if (ret == 0)
        kputs(buf.ptr, out);

    git_buf_dispose(&buf);
    git_describe_result_free(result);

    return ret;
}

static int get_branch_upstream_name(git_repository *repo, const char *branch_name, kstring_t *out) {
    git_reference *branch = NULL;
    git_reference *upstream = NULL;
    const char *name = NULL;
    int ret;

    ret = git_branch_lookup(&branch, repo, branch_name, GIT_BRANCH_LOCAL);
    if (ret < 0)
        return ret;

    ret = git_branch_upstream(&upstream, branch);
    git_reference_free(branch);
    if (ret < 0)
        return ret;

    ret = git_branch_name(&name, upstream);
    if (ret == 0)
        kputs(name, out);

    git_reference_free(upstream);
    return ret;
}

static int git_head_subject(git_repository *repo, kstring_t *out, const char *target) {
    git_reference *head = NULL;
    git_object *obj = NULL;
    git_commit *commit = NULL;
    int error;

    error = git_reference_lookup(&head, repo, target);
    if (error != 0)
        return error;

    error = git_reference_peel(&obj, head, GIT_OBJECT_COMMIT);
    git_reference_free(head);

    if (error != 0)
        return error;

    commit = (git_commit *)obj;

    kputs(git_commit_summary(commit), out);

    git_commit_free(commit);

    return 0;
}

static int git_symbolic_ref_short_head(git_repository *repo, kstring_t *out) {
    git_reference *head_ref = NULL;
    git_reference *resolved_ref = NULL;
    const char *target = NULL;
    int error;

    error = git_reference_lookup(&head_ref, repo, "HEAD");
    if (error != 0) {
        return error;
    }

    if (git_reference_type(head_ref) != GIT_REFERENCE_SYMBOLIC) {
        git_reference_free(head_ref);
        return GIT_ENOTFOUND;
    }

    target = git_reference_symbolic_target(head_ref);
    if (!target) {
        git_reference_free(head_ref);
        return GIT_ERROR;
    }

    error = git_reference_lookup(&resolved_ref, repo, target);
    if (error != 0) {
        const char *shorthand = target;
        if (strncmp(target, "refs/heads/", 11) == 0) {
            shorthand = target + 11;
        }
        kputs(shorthand, out);
        git_reference_free(head_ref);
        return 0;
    }

    kputs(git_reference_shorthand(resolved_ref), out);

    git_reference_free(resolved_ref);
    git_reference_free(head_ref);

    return 0;
}

static kstring_t *git_version_create(void) {
    int major, minor, rev;
    kstring_t *out = str_create(NULL, 0);
    char version[64];
    git_libgit2_version(&major, &minor, &rev);
    snprintf(version, sizeof(version), "git version %d.%d.%d", major, minor, rev);
    kputs(version, out);
    return out;
}

static int git_config_list_z(git_repository *repo, kstring_t *out) {
    git_config *cfg = NULL;
    git_config_iterator *iter = NULL;
    git_config_entry *entry = NULL;
    int error;

    error = git_repository_config(&cfg, repo);
    if (error != 0) {
        return error;
    }

    error = git_config_iterator_new(&iter, cfg);
    if (error != 0) {
        git_config_free(cfg);
        return error;
    }

    while ((error = git_config_next(&entry, iter)) == 0) {
        kputs(entry->name, out);
        kputc('\n', out);
        if (entry->value) {
            kputs(entry->value, out);
        }
        kputc('\0', out);
    }

    git_config_iterator_free(iter);
    git_config_free(cfg);

    return (error == GIT_ITEROVER) ? 0 : error;
}

static int git_rev_head(git_repository *repo, char result_buf[GIT_OID_MAX_HEXSIZE]) {
    git_reference *head = NULL;
    int error = git_repository_head(&head, repo);
    if (error != 0) {
        return error;
    }
    const git_oid *oid = git_reference_target(head);
    if (oid == NULL) {
        git_reference_free(head);
        return -1;
    }
    git_oid_tostr(result_buf, GIT_OID_MAX_HEXSIZE + 1, oid);
    git_reference_free(head);
    return 0;
}

static void git_root_worker(uv_work_t *req) {
    git_root_req_T *data = req->data;
    const char *root = git_repository_workdir(g_repo);
    char oid_str[GIT_OID_MAX_HEXSIZE];
    int rev_ret = git_rev_head(g_repo, oid_str);

    data->list_z = str_create(NULL, 0);
    int list_z_ret = git_config_list_z(g_repo, data->list_z);

    data->head_log_line = str_create(NULL, 0);
    data->subj = str_create(NULL, 0);
    int subj_ret = git_head_subject(g_repo, data->subj, "HEAD");

    data->branch = str_create(NULL, 0);
    int branch_ret = git_symbolic_ref_short_head(g_repo, data->branch);
    if (branch_ret != 0) {
        return;
    }

    data->upstream_branch = str_create(NULL, 0);
    int upstream_branch_ret = get_branch_upstream_name(g_repo, data->branch->s, data->upstream_branch);
    if (upstream_branch_ret != 0) {
        return;
    }

    data->tag_desc = str_create(NULL, 0);
    int tag_desc_ret = get_tag_desc(g_repo, data->tag_desc);
    if (tag_desc_ret != 0) {
        return;
    }

    data->upstream_subj = str_create(NULL, 0);
    char target_str[] = "refs/remotes/";
    STR_CLEANUP kstring_t *target = str_create(target_str, sizeof(target_str) - 1);
    kputs(data->upstream_branch->s, target);
    int upstream_subj_ret = git_head_subject(g_repo, data->upstream_subj, target->s);

    data->opt_tag_desc_head = str_create(NULL, 0);
    int opt_tag_desc_head_ret = get_tag_desc_if_match(g_repo, data->opt_tag_desc_head);

    data->worktree_porcelain = str_create(NULL, 0);
    int worktree_porcelain_ret = list_worktrees_porcelain(g_repo, data->worktree_porcelain);

    if (root && rev_ret == 0 && list_z_ret == 0 && subj_ret == 0 && upstream_subj_ret == 0 && opt_tag_desc_head_ret == 0 && worktree_porcelain_ret == 0) {
        data->root = str_create(root, strlen(root) - 1); // trim last slash
        data->rev_head = str_create(oid_str, GIT_OID_MAX_HEXSIZE);
        kputs(data->rev_head->s, data->head_log_line);
        kputs(" ", data->head_log_line);
        kputs(data->subj->s, data->head_log_line);
        data->version = git_version_create();

        data->result = 0;
    }
}

static void after_git_root(uv_work_t *req, int status) {
    GIT_ROOT_REQUEST_CLEANUP git_root_req_T *data = req->data;
    if (status != 0 || data->result < 0) {
        return;
    }
    magit_res_T res = {
        .id = data->id,
        .git_root = data->root,
        .rev_head = data->rev_head,
        .list_z = data->list_z,
        .branch = data->branch,
        .upstream_branch = data->upstream_branch,
        .head_log_line = data->head_log_line,
        .upstream_subj = data->upstream_subj,
        .tag_desc = data->tag_desc,
        .opt_tag_desc_head = data->opt_tag_desc_head,
        .version = data->version,
        .worktree_porcelain = data->worktree_porcelain,
        .dot_git_dir = g_dot_git_dir // global
    };
    msgpack_handler_send(&res);
}

int git_handler_queue_git_status(uv_loop_t *loop, u_int64_t id, kstring_t *pwd) {
    GIT_ROOT_REQUEST_CLEANUP git_root_req_T *data = malloc(sizeof(*data));
    data->pwd = pwd;
    data->id = id;
    data->result = -1;
    data->req.data = data;
    uv_work_t *req = &data->req;
    int ret = uv_queue_work(loop, req, git_root_worker, after_git_root);
    if (ret == 0) {
        xfer(data); // until after_git_root
    }
    return ret;
}

int git_handler_repo_init(const char *path) {
    git_buf gitdir = GIT_BUF_INIT;
    kstring_t *out = str_create(NULL, 0);
    int ret = git_repository_discover(&gitdir, path, 0, NULL);
    if (ret == 0) {
        kputs(gitdir.ptr, out);
        g_dot_git_dir = out;
        git_buf_dispose(&gitdir);
    }
    return git_repository_open_ext(&g_repo, path, 0, NULL);
}

void git_handler_repo_deinit() {
    git_repository_free(g_repo);
    if (g_dot_git_dir) {
        free(g_dot_git_dir->s);
        free(g_dot_git_dir);
    }
}
