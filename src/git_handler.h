int git_handler_queue_git_status(uv_loop_t *loop, u_int64_t id, kstring_t *pwd);
int git_handler_repo_init(const char *path);
void git_handler_repo_deinit();
