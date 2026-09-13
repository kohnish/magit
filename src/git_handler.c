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
        free(*req);
    }
};

#define GIT_ROOT_REQUEST_CLEANUP __attribute__((cleanup(git_root_request_cleanup)))

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
    if (root && rev_ret == 0 && list_z_ret == 0) {
        data->root = str_create(root, strlen(root));
        data->rev_head = str_create(oid_str, GIT_OID_MAX_HEXSIZE);
        data->result = 0;
    } else {
        // Bare repository
        data->result = 1;
    }
}

static void after_git_root(uv_work_t *req, int status) {
    GIT_ROOT_REQUEST_CLEANUP git_root_req_T *data = req->data;
    if (status != 0 || data->result < 0) {
        return;
    }
    magit_res_T res = {.id = data->id, .git_root = data->root, .rev_head = data->rev_head, .list_z = data->list_z};
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
