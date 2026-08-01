#include "msgpack_handler.h"
#include "util.h"
#include <git2.h>

typedef struct {
    uv_work_t req;
    uint64_t id;
    kstring_t *pwd;
    kstring_t *root;
    int result;
} git_root_request_t;

git_repository *g_repo = NULL;

static void git_root_request_cleanup(git_root_request_t **req) {
    if (req && *req) {
        if ((*req)->pwd) {
            free((*req)->pwd->s);
            free((*req)->pwd);
        }
        if ((*req)->root) {
            free((*req)->root->s);
            free((*req)->root);
        }
        free(*req);
    }
};

#define GIT_ROOT_REQUEST_CLEANUP __attribute__((cleanup(git_root_request_cleanup)))

static void git_root_worker(uv_work_t *req) {
    git_root_request_t *data = req->data;
    const char *root = git_repository_workdir(g_repo);
    if (root) {
        data->root = str_create(root, strlen(root));
        data->result = 0;
    } else {
        // Bare repository
        data->result = 1;
    }
}

static void after_git_root(uv_work_t *req, int status) {
    GIT_ROOT_REQUEST_CLEANUP git_root_request_t *data = req->data;
    if (status != 0 || data->result < 0) {
        return;
    }
    magit_res_T res = {.id = data->id, .git_root = data->root};
    msgpack_handler_send(&res);
}

int git_handler_queue_git_status(uv_loop_t *loop, u_int64_t id, kstring_t *pwd) {
    GIT_ROOT_REQUEST_CLEANUP git_root_request_t *data = malloc(sizeof(*data));
    data->pwd = pwd;
    data->root = NULL;
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
