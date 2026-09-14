/* { */
/*     "id": 1, */
/*     "cmd": 1 */

/* } */

#include "kstring.h"
#include <stdint.h>
#include <uv.h>

typedef struct magit_res_T {
    uint64_t id;
    kstring_t *git_root;
    kstring_t *rev_head;
    kstring_t *list_z;
    kstring_t *branch;
    kstring_t *upstream_branch;
    kstring_t *head_log_line;
    kstring_t *upstream_subj;
    kstring_t *tag_desc;
    kstring_t *opt_tag_desc_head;
    kstring_t *version;
    kstring_t *worktree_porcelain;
} magit_res_T;

#include <msgpack.h>

enum MAGIT_RES_KEY {
    MAGIT_RES_KEY_ID = 0,
    MAGIT_RES_KEY_GIT_ROOT,
    MAGIT_RES_KEY_REV_HEAD,
    MAGIT_RES_KEY_MAGIT_EXTENSION,
    MAGIT_RES_KEY_LIST_Z,
    MAGIT_RES_KEY_BRANCH,
    MAGIT_RES_KEY_HEAD_LOG_LINE,
    MAGIT_RES_KEY_UPSTREAM_BRANCH,
    MAGIT_RES_KEY_UPSTREAM_SUBJ,
    MAGIT_RES_KEY_TAG_DESC,
    MAGIT_RES_KEY_OPT_TAG_DESC_HEAD,
    MAGIT_RES_KEY_VERSION,
    MAGIT_RES_KEY_WORKTREE_PORCELAIN,
};

int msgpack_handler_recv(uv_loop_t *loop, const char *data, size_t len);
int msgpack_handler_send(magit_res_T *res);
