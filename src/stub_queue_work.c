#include "queue_work.h"
#include <stdlib.h>
#include <uv.h>

int queue_work(void *loop, task task_cb, after_task after_task_cb, void *data)
{
    (void)loop;
    uv_work_t *req = malloc(sizeof(*req));
    req->data = data;
    task(req);
    task_cb(req, 0);
    return 0;
}
