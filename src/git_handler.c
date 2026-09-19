#include "kstring.h"
#include "msgpack_handler.h"
#include "util.h"
#include <git2.h>
#include <git2/sys/errors.h>
#include <stdbool.h>
#include <stdint.h>

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
    kstring_t *config_status_show_untracked_files;
    kstring_t *status;
    kstring_t *diff;
    kstring_t *staged;
    uint64_t is_bare;
    kstring_t *stash;
    kstring_t *branches;
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
        if ((*req)->config_status_show_untracked_files) {
            free((*req)->config_status_show_untracked_files->s);
            free((*req)->config_status_show_untracked_files);
        }
        if ((*req)->status) {
            free((*req)->status->s);
            free((*req)->status);
        }
        if ((*req)->diff) {
            free((*req)->diff->s);
            free((*req)->diff);
        }
        if ((*req)->staged) {
            free((*req)->staged->s);
            free((*req)->staged);
        }
        if ((*req)->stash) {
            free((*req)->stash->s);
            free((*req)->stash);
        }
        if ((*req)->branches) {
            free((*req)->branches->s);
            free((*req)->branches);
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

#include <string.h>
#include <git2.h>
#include <git2/sys/errors.h>   /* git_error_clear() */

#define NUL(out) kputc('\0', (out))

static const char *shorten_upstream(const char *name) {
    if (strncmp(name, "refs/remotes/", sizeof("refs/remotes/") - 1) == 0)
        return name + sizeof("refs/remotes/") - 1;
    if (strncmp(name, "refs/heads/", sizeof("refs/heads/") - 1) == 0)
        return name + sizeof("refs/heads/") - 1;
    return name;
}

static int append_branch_record(git_repository *repo, git_reference *ref,
                                kstring_t *out) {
    const char *refname = git_reference_name(ref);
    const char *short_name = NULL;
    const char *up_name = NULL;
    const char *subject = "";
    git_buf up_buf = GIT_BUF_INIT;
    git_commit *commit = NULL;
    git_oid oid, up_oid;
    size_t ahead = 0, behind = 0;
    int track = 0;              /* 0 = none, 1 = counts, 2 = gone */
    int is_head, have_oid, ret;

    ret = git_branch_name(&short_name, ref);
    if (ret < 0)
        return ret;

    is_head = (git_branch_is_head(ref) == 1);

    /* Reads branch.<name>.remote/merge from config. Does NOT require the
     * upstream ref to exist, which is what lets us report [gone]. */
    ret = git_branch_upstream_name(&up_buf, repo, refname);
    if (ret == 0) {
        up_name = up_buf.ptr;
    } else if (ret == GIT_ENOTFOUND) {
        git_error_clear();
    } else {
        goto out;
    }

    /* name_to_id resolves symbolic refs too. */
    have_oid = (git_reference_name_to_id(&oid, repo, refname) == 0);
    if (!have_oid)
        git_error_clear();

    if (up_name && have_oid) {
        if (git_reference_name_to_id(&up_oid, repo, up_name) == 0) {
            ret = git_graph_ahead_behind(&ahead, &behind, repo, &oid, &up_oid);
            if (ret < 0)
                goto out;
            track = 1;
        } else {
            git_error_clear();
            track = 2;
        }
    }

    if (have_oid && git_commit_lookup(&commit, repo, &oid) == 0) {
        const char *s = git_commit_summary(commit);
        if (s)
            subject = s;
    } else {
        git_error_clear();
    }

    /* Everything is computed; now write the record. */
    kputs(is_head ? "*" : " ", out);          NUL(out);
    kputs(short_name, out);                   NUL(out);
    kputs(refname, out);                      NUL(out);
    kputs(up_name ? shorten_upstream(up_name) : "", out);  NUL(out);
    kputs(up_name ? up_name : "", out);       NUL(out);

    if (track == 1 && ahead && behind)
        ksprintf(out, "[ahead %zu, behind %zu]", ahead, behind);
    else if (track == 1 && ahead)
        ksprintf(out, "[ahead %zu]", ahead);
    else if (track == 1 && behind)
        ksprintf(out, "[behind %zu]", behind);
    else if (track == 2)
        kputs("[gone]", out);

    /* %00%00%00%00: one separator after track, two empty fields, and
     * one separator before subject. */
    NUL(out); NUL(out); NUL(out); NUL(out);

    kputs(subject, out);
    kputc('\n', out);
    ret = 0;

out:
    git_commit_free(commit);
    git_buf_dispose(&up_buf);
    return ret;
}

/* git for-each-ref '--format=%(HEAD)%00%(refname:short)%00%(refname)%00
 *   %(upstream:short)%00%(upstream)%00%(upstream:track)%00%00%00%00%(subject)'
 *   refs/heads */
static int get_branch_list(git_repository *repo, kstring_t *out) {
    git_branch_iterator *iter = NULL;
    git_reference *ref = NULL;
    git_branch_t type;
    int ret;

    ret = git_branch_iterator_new(&iter, repo, GIT_BRANCH_LOCAL);
    if (ret < 0)
        return ret;

    while ((ret = git_branch_next(&ref, &type, iter)) == 0) {
        ret = append_branch_record(repo, ref, out);
        git_reference_free(ref);
        ref = NULL;
        if (ret < 0)
            goto out;
    }
    if (ret == GIT_ITEROVER)
        ret = 0;

out:
    git_branch_iterator_free(iter);
    return ret;
}

/* git rev-parse --verify refs/stash
 * Appends "<oid>\n" on success; returns GIT_ENOTFOUND if no stash exists. */
static int get_stash_oid(git_repository *repo, kstring_t *out) {
    git_oid oid;
    char hex[GIT_OID_MAX_HEXSIZE + 1];
    int ret;

    ret = git_reference_name_to_id(&oid, repo, "refs/stash");
    if (ret < 0)
        return ret;

    git_oid_tostr(hex, sizeof(hex), &oid);
    kputs(hex, out);
    kputc('\n', out);
    return 0;
}
/* git diff --ita-visible-in-index --cached --no-ext-diff --no-prefix --
 * (HEAD tree -> index, no pathspec) */
static int get_staged(git_repository *repo, kstring_t *out) {
    git_index *index = NULL;
    git_reference *head_ref = NULL;
    git_object *head_obj = NULL;
    git_tree *head_tree = NULL;
    git_diff *diff = NULL;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
    git_buf buf = GIT_BUF_INIT;
    int ret;

    ret = git_repository_index(&index, repo);
    if (ret < 0)
        return ret;

    ret = git_index_read(index, 0);
    if (ret < 0)
        goto out;

    /* Unborn HEAD: leave head_tree NULL so we diff against the empty tree. */
    ret = git_repository_head(&head_ref, repo);
    if (ret == GIT_EUNBORNBRANCH || ret == GIT_ENOTFOUND) {
        git_error_clear();
    } else if (ret < 0) {
        goto out;
    } else {
        ret = git_reference_peel(&head_obj, head_ref, GIT_OBJECT_TREE);
        if (ret < 0)
            goto out;
        head_tree = (git_tree *)head_obj;
    }

    opts.flags = GIT_DIFF_NORMAL | GIT_DIFF_INDENT_HEURISTIC;
    opts.old_prefix = "";
    opts.new_prefix = "";

    ret = git_diff_tree_to_index(&diff, repo, head_tree, index, &opts);
    if (ret < 0)
        goto out;

    find_opts.flags = GIT_DIFF_FIND_RENAMES;
    ret = git_diff_find_similar(diff, &find_opts);
    if (ret < 0)
        goto out;

    ret = git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH);
    if (ret < 0)
        goto out;

    kputsn(buf.ptr, buf.size, out);

out:
    git_buf_dispose(&buf);
    git_diff_free(diff);
    git_object_free(head_obj);   /* frees head_tree too, same pointer */
    git_reference_free(head_ref);
    git_index_free(index);
    return ret;
}

/* git diff --ita-visible-in-index --no-ext-diff --no-prefix --
 * (index -> worktree, no pathspec) */
static int get_diff_patch(git_repository *repo, kstring_t *out) {
    git_diff *diff = NULL;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
    git_buf buf = GIT_BUF_INIT;
    int ret;

    /* git's default since 2.14; --ita-visible-in-index is also the default */
    opts.flags = GIT_DIFF_NORMAL | GIT_DIFF_INDENT_HEURISTIC;

    /* --no-prefix */
    opts.old_prefix = "";
    opts.new_prefix = "";

    /* NULL index = use the repo's own index. No pathspec = whole tree. */
    ret = git_diff_index_to_workdir(&diff, repo, NULL, &opts);
    if (ret < 0)
        goto out;

    /* diff.renames defaults to true in git */
    find_opts.flags = GIT_DIFF_FIND_RENAMES;
    ret = git_diff_find_similar(diff, &find_opts);
    if (ret < 0)
        goto out;

    ret = git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH);
    if (ret < 0)
        goto out;

    kputsn(buf.ptr, buf.size, out);

out:
    git_buf_dispose(&buf);
    git_diff_free(diff);
    return ret;
}

static int get_status_porcelain_z(git_repository *repo, kstring_t *out) {
    git_status_list *status = NULL;
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    size_t i, n;
    int ret;

    opts.version = GIT_STATUS_OPTIONS_VERSION;
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                 GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                 GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR |
                 GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;
    /* untracked-files=normal: do NOT set
     * GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS -- that corresponds to
     * --untracked-files=all and lists every file inside new dirs
     * instead of collapsing them to "dirname/". */

    ret = git_status_list_new(&status, repo, &opts);
    if (ret < 0)
        return ret;

    n = git_status_list_entrycount(status);
    for (i = 0; i < n; i++) {
        const git_status_entry *e = git_status_byindex(status, i);
        unsigned int s = e->status;
        char X = ' ', Y = ' ';
        const char *path = NULL;
        const char *old_path = NULL;

        if (s == GIT_STATUS_CURRENT)
            continue;

        if (s & GIT_STATUS_INDEX_NEW)          X = 'A';
        else if (s & GIT_STATUS_INDEX_MODIFIED)  X = 'M';
        else if (s & GIT_STATUS_INDEX_DELETED)   X = 'D';
        else if (s & GIT_STATUS_INDEX_RENAMED)   X = 'R';
        else if (s & GIT_STATUS_INDEX_TYPECHANGE) X = 'T';

        if (s & GIT_STATUS_WT_NEW)          Y = '?';
        else if (s & GIT_STATUS_WT_MODIFIED)  Y = 'M';
        else if (s & GIT_STATUS_WT_DELETED)   Y = 'D';
        else if (s & GIT_STATUS_WT_RENAMED)   Y = 'R';
        else if (s & GIT_STATUS_WT_TYPECHANGE) Y = 'T';

        /* untracked: index side stays '?' too, matching porcelain "??" */
        if ((s & GIT_STATUS_WT_NEW) &&
            !(s & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                   GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                   GIT_STATUS_INDEX_TYPECHANGE)))
            X = '?';

        if (s & GIT_STATUS_CONFLICTED)
            X = Y = 'U';

        /* pick path, preferring workdir diff (has the newest name) */
        if (e->index_to_workdir && e->index_to_workdir->new_file.path) {
            path = e->index_to_workdir->new_file.path;
            if (e->index_to_workdir->old_file.path &&
                strcmp(e->index_to_workdir->old_file.path, path) != 0)
                old_path = e->index_to_workdir->old_file.path;
        } else if (e->head_to_index && e->head_to_index->new_file.path) {
            path = e->head_to_index->new_file.path;
        }

        if (!old_path && e->head_to_index && e->head_to_index->old_file.path &&
            e->head_to_index->new_file.path &&
            strcmp(e->head_to_index->old_file.path,
                   e->head_to_index->new_file.path) != 0)
            old_path = e->head_to_index->old_file.path;

        if (!path)
            continue;

        kputc(X, out);
        kputc(Y, out);
        kputc(' ', out);
        kputs(path, out);
        kputc('\0', out);

        if (old_path) {
            kputs(old_path, out);
            kputc('\0', out);
        }
    }

    git_status_list_free(status);
    return 0;
}


static int get_config_status_show_untracked_files(git_repository *repo, kstring_t *out) {
    git_config *config = NULL;
    const char *value = NULL;
    int error;

    error = git_repository_config(&config, repo);
    if (error < 0)
        return error;

    error = git_config_get_string(&value, config, "status.showUntrackedFiles");

    if (error == 0 && value != NULL) {
        kputs(value, out);
    }
    git_config_free(config);
    return 0;
}

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

    data->config_status_show_untracked_files = str_create(NULL, 0);
    int config_status_show_untracked_files_ret = get_config_status_show_untracked_files(g_repo, data->config_status_show_untracked_files);

    data->status = str_create(NULL, 0);
    int status_ret = get_status_porcelain_z(g_repo, data->status);

    data->diff = str_create(NULL, 0);
    int diff_ret = get_diff_patch(g_repo, data->diff);

    data->staged = str_create(NULL, 0);
    int staged_ret = get_staged(g_repo, data->staged);

    data->is_bare = git_repository_is_bare(g_repo) ? 1 : 0;

    data->stash = str_create(NULL, 0);
    get_stash_oid(g_repo, data->stash);

    data->branches = str_create(NULL, 0);
    get_branch_list(g_repo, data->branches);

    if (root && rev_ret == 0 && list_z_ret == 0 && subj_ret == 0 && upstream_subj_ret == 0 && opt_tag_desc_head_ret == 0 && worktree_porcelain_ret == 0 && config_status_show_untracked_files_ret == 0 && status_ret == 0) {
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
        .dot_git_dir = g_dot_git_dir, // global
        .config_status_show_untracked_files = data->config_status_show_untracked_files,
        .status = data->status,
        .diff = data->diff,
        .staged = data->staged,
        .is_bare = data->is_bare,
        .stash = data->stash,
        .branches = data->branches,
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
