#include <stdlib.h>
#include <uv.h>

int queue_work(uv_loop_t *loop, task task_cb, after_task after_task_cb, void *arg) {
    uv_work_t *req = malloc(sizeof(*req));
    req->data = data;
    int ret = uv_queue_work(loop, req, task_cb, after_task_cb);
    if (ret != 0) {
        free(req);
        return -1;
    }
    return 0;
}
