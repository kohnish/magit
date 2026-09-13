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
} magit_res_T;

#include <msgpack.h>

enum MAGIT_RES_KEY {
    MAGIT_RES_KEY_ID = 0,
    MAGIT_RES_KEY_GIT_ROOT,
    MAGIT_RES_KEY_REV_HEAD,
    MAGIT_RES_KEY_MAGIT_EXTENSION,
    MAGIT_RES_KEY_LIST_Z,
};

int msgpack_handler_recv(uv_loop_t *loop, const char *data, size_t len);
int msgpack_handler_send(magit_res_T *res);
