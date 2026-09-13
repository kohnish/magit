#include "kstring.h"
#include "msgpack_handler.h"
#include "util.h"
#include <git2.h>

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
    int result;
} git_root_req_T;

git_repository *g_repo = NULL;

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
        if ((*req)->head_log_line) {
            free((*req)->head_log_line->s);
            free((*req)->head_log_line);
        }
        free(*req);
    }
};

#define GIT_ROOT_REQUEST_CLEANUP __attribute__((cleanup(git_root_request_cleanup)))

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

static int git_head_subject(git_repository *repo, kstring_t *out) {
    git_reference *head = NULL;
    git_object *obj = NULL;
    git_commit *commit = NULL;
    int error;

    error = git_reference_lookup(&head, repo, "HEAD");
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
    STR_CLEANUP kstring_t *tmp_subj = str_create(NULL, 0);
    int subj_ret = git_head_subject(g_repo, tmp_subj);

    data->branch = str_create(NULL, 0);
    int branch_ret = git_symbolic_ref_short_head(g_repo, data->branch);
    if (branch_ret != 0) {
        return;
    }

    data->upstream_branch = str_create(NULL, 0);
    int upstream_branch_ret = get_branch_upstream_name(g_repo, data->branch->s, data->upstream_branch);

    if (root && rev_ret == 0 && list_z_ret == 0 && subj_ret == 0 && upstream_branch_ret == 0) {
        data->root = str_create(root, strlen(root));
        data->rev_head = str_create(oid_str, GIT_OID_MAX_HEXSIZE);
        kputs(data->rev_head->s, data->head_log_line);
        kputs(" ", data->head_log_line);
        kputs(tmp_subj->s, data->head_log_line);
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
        .head_log_line = data->head_log_line
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
    return git_repository_open_ext(&g_repo, path, 0, NULL);
}

void git_handler_repo_deinit() {
    git_repository_free(g_repo);
}
