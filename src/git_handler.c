#include "kstring.h"
#include "msgpack_handler.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <git2.h>
#include <git2/sys/errors.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

/* ========================================================================
 * Logging
 *
 * Configuration (environment variables, read once on first log call):
 *   GIT_HANDLER_LOG_LEVEL  debug | info | warn | error | off   (default: info)
 *   GIT_HANDLER_LOG_FILE   path to append to                    (default: stderr)
 *
 * Every message is formatted into a local buffer and written with a single
 * fwrite(), so lines from the libuv worker threads and the main thread do not
 * interleave. Never log to stdout: it may carry the msgpack stream.
 * ======================================================================== */

typedef enum {
    GH_LOG_LEVEL_DEBUG = 0,
    GH_LOG_LEVEL_INFO,
    GH_LOG_LEVEL_WARN,
    GH_LOG_LEVEL_ERROR,
    GH_LOG_LEVEL_OFF,
} gh_log_level_t;

/* A worker step slower than this is reported at WARN instead of DEBUG. */
#define GH_SLOW_STEP_MS 500

static gh_log_level_t g_log_level = GH_LOG_LEVEL_INFO;
static FILE *g_log_fp = NULL; /* NULL means stderr */
static pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

static const char *gh_log_level_name(gh_log_level_t level) {
    switch (level) {
    case GH_LOG_LEVEL_DEBUG: return "DEBUG";
    case GH_LOG_LEVEL_INFO:  return "INFO";
    case GH_LOG_LEVEL_WARN:  return "WARN";
    case GH_LOG_LEVEL_ERROR: return "ERROR";
    default:                 return "?";
    }
}

static gh_log_level_t gh_log_parse_level(const char *s, gh_log_level_t fallback) {
    if (!s)
        return fallback;
    if (strcasecmp(s, "debug") == 0)
        return GH_LOG_LEVEL_DEBUG;
    if (strcasecmp(s, "info") == 0)
        return GH_LOG_LEVEL_INFO;
    if (strcasecmp(s, "warn") == 0 || strcasecmp(s, "warning") == 0)
        return GH_LOG_LEVEL_WARN;
    if (strcasecmp(s, "error") == 0)
        return GH_LOG_LEVEL_ERROR;
    if (strcasecmp(s, "off") == 0 || strcasecmp(s, "none") == 0)
        return GH_LOG_LEVEL_OFF;
    return fallback;
}

static void gh_log_init_once(void) {
    const char *path = getenv("GIT_HANDLER_LOG_FILE");

    g_log_level = gh_log_parse_level(getenv("GIT_HANDLER_LOG_LEVEL"), g_log_level);

    if (path && *path) {
        FILE *fp = fopen(path, "a");
        if (fp) {
            setvbuf(fp, NULL, _IOLBF, 0);
            g_log_fp = fp;
        } else {
            fprintf(stderr, "git_handler: cannot open log file '%s': %s\n",
                    path, strerror(errno));
        }
    }
}

static void gh_log(gh_log_level_t level, const char *func, int line,
                   const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void gh_log(gh_log_level_t level, const char *func, int line,
                   const char *fmt, ...) {
    char msg[1024];
    char buf[1280];
    char ts[16];
    struct timespec now;
    struct tm tm;
    va_list ap;
    int n;

    pthread_once(&g_log_once, gh_log_init_once);
    if (level < g_log_level)
        return;

    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    n = snprintf(buf, sizeof(buf), "%s.%03ld [%-5s] [%lu] %s:%d: %s\n",
                 ts, now.tv_nsec / 1000000L, gh_log_level_name(level),
                 (unsigned long)(uintptr_t)pthread_self(), func, line, msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf)) {   /* truncated: keep the trailing newline */
        n = (int)sizeof(buf) - 1;
        buf[n - 1] = '\n';
    }
    fwrite(buf, 1, (size_t)n, g_log_fp ? g_log_fp : stderr);
}

#define GH_LOG(level, ...) gh_log((level), __func__, __LINE__, __VA_ARGS__)
#define GH_LOG_DEBUG(...)  GH_LOG(GH_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define GH_LOG_INFO(...)   GH_LOG(GH_LOG_LEVEL_INFO,  __VA_ARGS__)
#define GH_LOG_WARN(...)   GH_LOG(GH_LOG_LEVEL_WARN,  __VA_ARGS__)
#define GH_LOG_ERROR(...)  GH_LOG(GH_LOG_LEVEL_ERROR, __VA_ARGS__)

/* "Not found" and "unborn branch" are normal outcomes for many lookups
 * (no stash, no upstream, fresh repo), so they log at DEBUG, not ERROR. */
static gh_log_level_t gh_git_err_level(int code) {
    return (code == GIT_ENOTFOUND || code == GIT_EUNBORNBRANCH)
               ? GH_LOG_LEVEL_DEBUG
               : GH_LOG_LEVEL_ERROR;
}

static void gh_log_git_error(gh_log_level_t level, const char *func, int line,
                             int code, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Logs "<what> failed: code=N: <libgit2 message>". Call it BEFORE
 * git_error_clear(), otherwise the message is gone. */
static void gh_log_git_error(gh_log_level_t level, const char *func, int line,
                             int code, const char *fmt, ...) {
    const git_error *err = git_error_last();
    char what[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);

    gh_log(level, func, line, "%s failed: code=%d: %s", what, code,
           (err && err->message) ? err->message : "no libgit2 error message");
}

#define GH_LOG_GIT_ERR(code, ...) \
    gh_log_git_error(gh_git_err_level(code), __func__, __LINE__, (code), __VA_ARGS__)
#define GH_LOG_GIT_ERR_AT(level, code, ...) \
    gh_log_git_error((level), __func__, __LINE__, (code), __VA_ARGS__)

/* Log and return / goto when a libgit2 call returned < 0. */
#define GH_CHECK_RET(ret, ...)                     \
    do {                                           \
        if ((ret) < 0) {                           \
            GH_LOG_GIT_ERR((ret), __VA_ARGS__);    \
            return (ret);                          \
        }                                          \
    } while (0)

#define GH_CHECK_GOTO(label, ret, ...)             \
    do {                                           \
        if ((ret) < 0) {                           \
            GH_LOG_GIT_ERR((ret), __VA_ARGS__);    \
            goto label;                            \
        }                                          \
    } while (0)

/* ---- per-step timing for the worker ---- */

static uint64_t gh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void gh_log_step(const char *func, int line, const char *name, int rc,
                        uint64_t ms, int optional) {
    int slow = ms >= GH_SLOW_STEP_MS;
    gh_log_level_t level = GH_LOG_LEVEL_DEBUG;

    if ((rc != 0 && !optional) || slow)
        level = GH_LOG_LEVEL_WARN;
    gh_log(level, func, line, "step %s: rc=%d in %" PRIu64 " ms%s", name, rc, ms,
           slow ? " (slow)" : "");
}

/* Runs `call`, stores its return value in rc_var, and logs rc + duration.
 * GH_STEP_OPT is for steps whose failure is expected (no stash, short history). */
#define GH_STEP_IMPL(rc_var, name, optional, call)                          \
    do {                                                                    \
        uint64_t t0_ = gh_now_ms();                                         \
        git_error_clear();                                                  \
        (rc_var) = (call);                                                  \
        gh_log_step(__func__, __LINE__, (name), (rc_var), gh_now_ms() - t0_, \
                    (optional));                                            \
    } while (0)
#define GH_STEP(rc_var, name, call)     GH_STEP_IMPL(rc_var, name, 0, call)
#define GH_STEP_OPT(rc_var, name, call) GH_STEP_IMPL(rc_var, name, 1, call)

/* ======================================================================== */

typedef struct {
    uv_work_t req;
    uint64_t id;
    kstring_t *pwd;
    kstring_t *repo_path;
    kstring_t *dot_git_dir;
    git_repository *repo;
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
    kstring_t *revision_by_idx;
    kstring_t *logs;
    kstring_t *rev_short_head;
    kstring_t *tag_master;
    kstring_t *tag_origin_master;
    kstring_t *origin_head;
    kstring_t *tag_origin_head;
    kstring_t *upstream_branch_master;
    int result;
} git_root_req_T;

git_repository *g_repo = NULL;
kstring_t *g_dot_git_dir = NULL;

/* Forward declarations for task wrapper functions */
static int task_rev_head(git_repository *repo, kstring_t *out, void *arg);
static int task_config_list_z(git_repository *repo, kstring_t *out, void *arg);
static int task_head_subject(git_repository *repo, kstring_t *out, void *arg);
static int task_symbolic_ref_short_head(git_repository *repo, kstring_t *out, void *arg);
static int task_branch_upstream_name(git_repository *repo, kstring_t *out, void *arg);
static int task_tag_desc(git_repository *repo, kstring_t *out, void *arg);
static int task_tag_desc_if_match(git_repository *repo, kstring_t *out, void *arg);
static int task_worktrees_porcelain(git_repository *repo, kstring_t *out, void *arg);
static int task_config_show_untracked(git_repository *repo, kstring_t *out, void *arg);
static int task_status_porcelain_z(git_repository *repo, kstring_t *out, void *arg);
static int task_diff_patch(git_repository *repo, kstring_t *out, void *arg);
static int task_staged(git_repository *repo, kstring_t *out, void *arg);
static int task_stash_oid(git_repository *repo, kstring_t *out, void *arg);
static int task_branch_list(git_repository *repo, kstring_t *out, void *arg);
static int task_rev_parse_verify(git_repository *repo, kstring_t *out, void *arg);
static int task_log(git_repository *repo, kstring_t *out, void *arg);
static int task_rev_parse_short_head(git_repository *repo, kstring_t *out, void *arg);
static int task_tag_master(git_repository *repo, kstring_t *out, void *arg);
static int task_tag_origin_master(git_repository *repo, kstring_t *out, void *arg);
static int task_tag_origin_head(git_repository *repo, kstring_t *out, void *arg);
static int task_origin_head(git_repository *repo, kstring_t *out, void *arg);
static int task_upstream_branch_master(git_repository *repo, kstring_t *out, void *arg);
static int task_upstream_subj(git_repository *repo, kstring_t *out, void *arg);

static void git_root_request_cleanup(git_root_req_T **req) {
    if (req && *req) {
        if ((*req)->repo) {
            git_repository_free((*req)->repo);
        }
        if ((*req)->pwd) {
            free((*req)->pwd->s);
            free((*req)->pwd);
        }
        if ((*req)->repo_path) {
            free((*req)->repo_path->s);
            free((*req)->repo_path);
        }
        if ((*req)->dot_git_dir) {
            free((*req)->dot_git_dir->s);
            free((*req)->dot_git_dir);
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
        if ((*req)->revision_by_idx) {
            free((*req)->revision_by_idx->s);
            free((*req)->revision_by_idx);
        }
        if ((*req)->upstream_branch) {
            free((*req)->upstream_branch->s);
            free((*req)->upstream_branch);
        }
        if ((*req)->head_log_line) {
            free((*req)->head_log_line->s);
            free((*req)->head_log_line);
        }
        if ((*req)->tag_desc) {
            free((*req)->tag_desc->s);
            free((*req)->tag_desc);
        }
        if ((*req)->logs) {
            free((*req)->logs->s);
            free((*req)->logs);
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <git2.h>
#include <git2/sys/errors.h>
#include "kstring.h"

#define FF '\x0c'
#define CLEANUP(fn) __attribute__((cleanup(fn)))

#define DEFINE_CLEANUP(type, free_fn)                        \
    static inline void cleanup_##type(type **p) {            \
        if (*p)                                              \
            free_fn(*p);                                     \
    }

DEFINE_CLEANUP(git_reference,          git_reference_free)
DEFINE_CLEANUP(git_object,             git_object_free)

static inline void cleanup_git_buf(git_buf *b) { git_buf_dispose(b); }

/* git rev-parse --verify --abbrev-ref <branch>@{upstream}
 * Appends "<upstream-short-name>\n" (e.g. "origin/master") on success.
 * Returns GIT_ENOTFOUND if the branch doesn't exist, has no upstream
 * configured, or its upstream ref is gone (git exits 128 in all three). */
static int get_upstream_branch(git_repository *repo, const char *branch_name, kstring_t *out) {
    CLEANUP(cleanup_git_reference) git_reference *branch = NULL;
    CLEANUP(cleanup_git_reference) git_reference *upstream = NULL;
    const char *short_name = NULL;
    int ret;

    ret = git_branch_lookup(&branch, repo, branch_name, GIT_BRANCH_LOCAL);
    GH_CHECK_RET(ret, "git_branch_lookup");            /* ENOTFOUND -> DEBUG */

    ret = git_branch_upstream(&upstream, branch);
    GH_CHECK_RET(ret, "git_branch_upstream");          /* ENOTFOUND -> DEBUG */

    /* Strips "refs/remotes/" or "refs/heads/". The string is owned by
     * `upstream`, so copy it out before the cleanup runs on return. */
    ret = git_branch_name(&short_name, upstream);
    GH_CHECK_RET(ret, "git_branch_name");

    kputs(short_name, out);
    kputc('\n', out);
    return 0;
}
/* git rev-parse --verify --abbrev-ref master@{upstream} */
/* usage: get_upstream_short(repo, "master", out); */


/* git rev-parse --short HEAD
 * Appends "<abbrev-oid>\n" on success. Returns < 0 on unborn HEAD. */
static int get_rev_parse_short_head(git_repository *repo, kstring_t *out) {
    CLEANUP(cleanup_git_object) git_object *obj = NULL;
    CLEANUP(cleanup_git_buf)    git_buf sid = GIT_BUF_INIT;
    int ret;

    ret = git_revparse_single(&obj, repo, "HEAD");
    if (ret < 0)
        return ret;

    ret = git_object_short_id(&sid, obj);
    if (ret < 0)
        return ret;

    kputs(sid.ptr, out);
    kputc('\n', out);
    return 0;
}

/* git symbolic-ref <refname>
 * Appends "<target-refname>\n" on success. Returns GIT_ENOTFOUND if the ref
 * doesn't exist or isn't symbolic (git exits 128 in both cases). */
static int get_symbolic_ref(git_repository *repo, const char *refname,
                            kstring_t *out) {
    CLEANUP(cleanup_git_reference) git_reference *ref = NULL;
    const char *target;
    int ret;

    ret = git_reference_lookup(&ref, repo, refname);
    GH_CHECK_RET(ret, "git_reference_lookup");   /* ENOTFOUND -> DEBUG */

    if (git_reference_type(ref) != GIT_REFERENCE_SYMBOLIC)
        return GIT_ENOTFOUND;

    /* Owned by ref, so copy it out before ref is freed on return. */
    target = git_reference_symbolic_target(ref);
    if (!target)
        return GIT_ENOTFOUND;

    kputs(target, out);
    kputc('\n', out);
    return 0;
}

/* git symbolic-ref refs/remotes/origin/HEAD */
static int get_origin_head(git_repository *repo, kstring_t *out) {
    return get_symbolic_ref(repo, "refs/remotes/origin/HEAD", out);
}

/* Git's commit-graph optimizations keep these history queries fast on large repositories. */
static int run_git_capture(git_repository *repo, kstring_t *out,
                           char *const argv[]) {
    const char *path = git_repository_workdir(repo);
    size_t output_start = out->l;
    int stdout_pipe[2];
    int stderr_pipe[2];
    char error_output[2048];
    size_t error_length = 0;
    pid_t pid;
    int status;
    int ret = 0;
    int stdout_open = 1;
    int stderr_open = 1;

    if (!path)
        path = git_repository_path(repo);
    if (!path) {
        GH_LOG_ERROR("repository has no path for Git subprocess");
        return -1;
    }
    if (pipe(stdout_pipe) < 0) {
        GH_LOG_ERROR("pipe failed: %s", strerror(errno));
        return -1;
    }
    if (pipe(stderr_pipe) < 0) {
        int saved_errno = errno;
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        GH_LOG_ERROR("pipe failed: %s", strerror(saved_errno));
        return -1;
    }
    if (fcntl(stdout_pipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(stdout_pipe[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(stderr_pipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(stderr_pipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        int saved_errno = errno;
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        GH_LOG_ERROR("fcntl(FD_CLOEXEC) failed: %s", strerror(saved_errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        GH_LOG_ERROR("fork failed: %s", strerror(saved_errno));
        return -1;
    }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0)
            _exit(126);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    while (stdout_open || stderr_open) {
        struct pollfd fds[] = {
            { .fd = stdout_open ? stdout_pipe[0] : -1, .events = POLLIN },
            { .fd = stderr_open ? stderr_pipe[0] : -1, .events = POLLIN }
        };
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            GH_LOG_ERROR("poll for Git subprocess output failed: %s",
                         strerror(errno));
            ret = -1;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char buffer[8192];
            ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
            if (n > 0) {
                if (i == 0) {
                    kputsn(buffer, (size_t)n, out);
                } else {
                    size_t copy = sizeof(error_output) - 1 - error_length;
                    if (copy > (size_t)n)
                        copy = (size_t)n;
                    memcpy(error_output + error_length, buffer, copy);
                    error_length += copy;
                }
            } else if (n == 0) {
                if (i == 0) {
                    close(stdout_pipe[0]);
                    stdout_open = 0;
                } else {
                    close(stderr_pipe[0]);
                    stderr_open = 0;
                }
            } else if (errno != EINTR) {
                GH_LOG_ERROR("reading Git subprocess output failed: %s",
                             strerror(errno));
                ret = -1;
                break;
            }
        }
        if (ret < 0)
            break;
    }
    if (stdout_open)
        close(stdout_pipe[0]);
    if (stderr_open)
        close(stderr_pipe[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (out->s) {
            out->l = output_start;
            out->s[output_start] = '\0';
        }
        GH_LOG_ERROR("waitpid for Git subprocess failed: %s", strerror(errno));
        return -1;
    }
    error_output[error_length] = '\0';
    if (ret < 0) {
        if (out->s) {
            out->l = output_start;
            out->s[output_start] = '\0';
        }
        return ret;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (out->s) {
            out->l = output_start;
            out->s[output_start] = '\0';
        }
        GH_LOG_DEBUG("Git subprocess failed (status=%d): %s",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                     error_output);
        return -1;
    }
    return 0;
}

/* git log --format=%h%x0c%D%x0c%x0c%aN%x0c%at%x0c%s --decorate=full
 *         -n<limit> --use-mailmap --no-prefix --                      */
static int get_log(git_repository *repo, size_t limit, kstring_t *out) {
    char limit_arg[32];
    char format_arg[] = "--format=%h%x0c%D%x0c%x0c%aN%x0c%at%x0c%s";
    const char *path = git_repository_workdir(repo);
    if (!path)
        path = git_repository_path(repo);
    char *const argv[] = {
        "git", "--no-pager", "-C", (char *)path,
        "-c", "core.preloadindex=true",
        "-c", "log.showSignature=false",
        "-c", "color.ui=false",
        "-c", "color.diff=false",
        "log", format_arg, "--decorate=full",
        "-n", limit_arg, "--use-mailmap", "--no-prefix", "--",
        NULL
    };
    snprintf(limit_arg, sizeof(limit_arg), "%zu", limit);
    return run_git_capture(repo, out, argv);
}

/* usage: get_log(repo, 30, out); */
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
    GH_CHECK_GOTO(out, ret, "git_branch_name(%s)", refname);

    is_head = (git_branch_is_head(ref) == 1);

    /* Reads branch.<name>.remote/merge from config. Does NOT require the
     * upstream ref to exist, which is what lets us report [gone]. */
    ret = git_branch_upstream_name(&up_buf, repo, refname);
    if (ret == 0) {
        up_name = up_buf.ptr;
    } else if (ret == GIT_ENOTFOUND) {
        git_error_clear();
    } else {
        GH_LOG_GIT_ERR(ret, "git_branch_upstream_name(%s)", refname);
        goto out;
    }

    /* name_to_id resolves symbolic refs too. */
    have_oid = (git_reference_name_to_id(&oid, repo, refname) == 0);
    if (!have_oid) {
        GH_LOG_DEBUG("cannot resolve %s to an oid", refname);
        git_error_clear();
    }

    if (up_name && have_oid) {
        if (git_reference_name_to_id(&up_oid, repo, up_name) == 0) {
            ret = git_graph_ahead_behind(&ahead, &behind, repo, &oid, &up_oid);
            GH_CHECK_GOTO(out, ret, "git_graph_ahead_behind(%s, %s)", refname, up_name);
            track = 1;
        } else {
            GH_LOG_DEBUG("upstream %s of %s is gone", up_name, refname);
            git_error_clear();
            track = 2;
        }
    }

    if (have_oid && git_commit_lookup(&commit, repo, &oid) == 0) {
        const char *s = git_commit_summary(commit);
        if (s)
            subject = s;
    } else {
        GH_LOG_DEBUG("no commit summary available for %s", refname);
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
    if (ret < 0) {
        GH_LOG_GIT_ERR(ret, "git_branch_iterator_new");
        return ret;
    }

    while ((ret = git_branch_next(&ref, &type, iter)) == 0) {
        ret = append_branch_record(repo, ref, out);
        if (ret < 0)
            GH_LOG_ERROR("append_branch_record(%s) failed: code=%d",
                         git_reference_name(ref), ret);
        git_reference_free(ref);
        ref = NULL;
        if (ret < 0)
            goto out;
    }
    if (ret == GIT_ITEROVER)
        ret = 0;
    else
        GH_LOG_GIT_ERR(ret, "git_branch_next");

out:
    git_branch_iterator_free(iter);
    return ret;
}

/* git rev-parse --verify <full-refname>
 * Appends "<oid>\n" on success; returns GIT_ENOTFOUND if the ref doesn't exist. */
static int get_ref_oid(git_repository *repo, const char *refname, kstring_t *out) {
    git_oid oid;
    char hex[GIT_OID_MAX_HEXSIZE + 1];
    int ret;

    ret = git_reference_name_to_id(&oid, repo, refname);
    GH_CHECK_RET(ret, "git_reference_name_to_id");   /* ENOTFOUND -> DEBUG */

    git_oid_tostr(hex, sizeof(hex), &oid);
    kputs(hex, out);
    kputc('\n', out);
    return 0;
}

/* git rev-parse --verify refs/stash */
static int get_stash_oid(git_repository *repo, kstring_t *out) {
    return get_ref_oid(repo, "refs/stash", out);
}

/* git rev-parse --verify refs/tags/master */
static int get_tag_master_oid(git_repository *repo, kstring_t *out) {
    return get_ref_oid(repo, "refs/tags/master", out);
}

/* git rev-parse --verify <spec>
 * Appends "<oid>\n" on success. Returns < 0 if the spec doesn't resolve. */
static int get_rev_parse_verify(git_repository *repo, const char *spec,
                                kstring_t *out) {
    git_object *obj = NULL;
    char hex[GIT_OID_MAX_HEXSIZE + 1];
    int ret;

    ret = git_revparse_single(&obj, repo, spec);
    GH_CHECK_RET(ret, "git_revparse_single(%s)", spec);

    git_oid_tostr(hex, sizeof(hex), git_object_id(obj));
    kputs(hex, out);
    kputc('\n', out);

    git_object_free(obj);
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
    GH_CHECK_RET(ret, "git_repository_index");

    ret = git_index_read(index, 0);
    GH_CHECK_GOTO(out, ret, "git_index_read");

    /* Unborn HEAD: leave head_tree NULL so we diff against the empty tree. */
    ret = git_repository_head(&head_ref, repo);
    if (ret == GIT_EUNBORNBRANCH || ret == GIT_ENOTFOUND) {
        GH_LOG_DEBUG("HEAD is unborn, diffing index against the empty tree");
        git_error_clear();
    } else if (ret < 0) {
        GH_LOG_GIT_ERR(ret, "git_repository_head");
        goto out;
    } else {
        ret = git_reference_peel(&head_obj, head_ref, GIT_OBJECT_TREE);
        GH_CHECK_GOTO(out, ret, "git_reference_peel(HEAD -> tree)");
        head_tree = (git_tree *)head_obj;
    }

    opts.flags = GIT_DIFF_NORMAL | GIT_DIFF_INDENT_HEURISTIC;
    opts.old_prefix = "";
    opts.new_prefix = "";

    ret = git_diff_tree_to_index(&diff, repo, head_tree, index, &opts);
    GH_CHECK_GOTO(out, ret, "git_diff_tree_to_index");

    find_opts.flags = GIT_DIFF_FIND_RENAMES;
    ret = git_diff_find_similar(diff, &find_opts);
    GH_CHECK_GOTO(out, ret, "git_diff_find_similar");

    ret = git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH);
    GH_CHECK_GOTO(out, ret, "git_diff_to_buf");

    kputsn(buf.ptr, buf.size, out);
    GH_LOG_DEBUG("staged diff: %zu bytes", (size_t)buf.size);

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
    GH_CHECK_GOTO(out, ret, "git_diff_index_to_workdir");

    /* diff.renames defaults to true in git */
    find_opts.flags = GIT_DIFF_FIND_RENAMES;
    ret = git_diff_find_similar(diff, &find_opts);
    GH_CHECK_GOTO(out, ret, "git_diff_find_similar");

    ret = git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH);
    GH_CHECK_GOTO(out, ret, "git_diff_to_buf");

    kputsn(buf.ptr, buf.size, out);
    GH_LOG_DEBUG("worktree diff: %zu bytes", (size_t)buf.size);

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
    GH_CHECK_RET(ret, "git_status_list_new");

    n = git_status_list_entrycount(status);
    GH_LOG_DEBUG("status: %zu entries", n);
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

        if (!path) {
            GH_LOG_DEBUG("status entry %zu has no path (flags=0x%x), skipped", i, s);
            continue;
        }

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
    git_buf value = GIT_BUF_INIT;
    int error;

    error = git_repository_config(&config, repo);
    if (error < 0) {
        GH_LOG_GIT_ERR(error, "git_repository_config");
        return error;
    }

    /* git_config_get_string() rejects a live config object; the _buf variant
     * works on it and copies the value, so no snapshot is needed. */
    error = git_config_get_string_buf(&value, config, "status.showUntrackedFiles");

    if (error == 0 && value.ptr != NULL) {
        kputs(value.ptr, out);
    } else if (error < 0) {
        /* ENOTFOUND (option unset) is the common case and logs at DEBUG. */
        GH_LOG_GIT_ERR(error, "git_config_get_string_buf(status.showUntrackedFiles)");
    }
    git_buf_dispose(&value);
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
        int head_ret;

        kputs("worktree ", out);
        kputs(main_path, out);
        kputc(0, out);

        head_ret = git_repository_head(&head_ref, repo);
        if (head_ret == 0) {
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
        } else {
            /* unborn HEAD logs at DEBUG; anything else at ERROR */
            GH_LOG_GIT_ERR(head_ret, "git_repository_head (main worktree %s)", main_path);
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
    if (ret < 0) {
        GH_LOG_GIT_ERR(ret, "git_worktree_list");
        return ret;
    }
    GH_LOG_DEBUG("%zu linked worktree(s)", wt_names.count);

    for (size_t i = 0; i < wt_names.count; i++) {
        git_worktree *wt = NULL;
        git_repository *wt_repo = NULL;
        git_buf lock_reason = GIT_BUF_INIT;
        int r;

        r = git_worktree_lookup(&wt, repo, wt_names.strings[i]);
        if (r < 0) {
            GH_LOG_GIT_ERR_AT(GH_LOG_LEVEL_WARN, r, "git_worktree_lookup(%s)",
                              wt_names.strings[i]);
            continue;
        }

        kputs("worktree ", out);
        kputs(git_worktree_path(wt), out);
        kputc(0, out);

        r = git_repository_open_from_worktree(&wt_repo, wt);
        if (r == 0) {
            int head_ret = git_repository_head(&head_ref, wt_repo);
            if (head_ret == 0) {
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
            } else {
                GH_LOG_GIT_ERR(head_ret, "git_repository_head (worktree %s)",
                               wt_names.strings[i]);
            }
            git_repository_free(wt_repo);
        } else {
            GH_LOG_GIT_ERR_AT(GH_LOG_LEVEL_WARN, r,
                              "git_repository_open_from_worktree(%s)",
                              wt_names.strings[i]);
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

    if (git_object_lookup(&obj, ctx->repo, oid, GIT_OBJECT_ANY) < 0) {
        GH_LOG_DEBUG("tag %s: object lookup failed, skipping", name);
        return 0;
    }

    if (git_object_peel(&commit, obj, GIT_OBJECT_COMMIT) == 0 &&
        git_oid_equal(git_object_id(commit), ctx->target)) {
        const char *n = name;
        if (strncmp(n, "refs/tags/", 10) == 0)
            n += 10;
        ctx->match = strdup(n);
        if (!ctx->match)
            GH_LOG_ERROR("strdup failed for tag %s", n);
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
    GH_CHECK_RET(ret, "git_revparse_single(HEAD)");

    ctx.target = git_object_id(head);

    ret = git_tag_foreach(repo, tag_cb, &ctx);
    git_object_free(head);
    if (ret < 0 && ret != GIT_ENOTFOUND) {
        GH_LOG_GIT_ERR(ret, "git_tag_foreach");
        return ret;
    }

    if (ctx.match) {
        GH_LOG_DEBUG("HEAD is tagged: %s", ctx.match);
        kputs(ctx.match, out);
        free(ctx.match);
    }

    return 0;
}

static int get_tag_desc(git_repository *repo, kstring_t *out) {
    const char *path = git_repository_workdir(repo);
    if (!path)
        path = git_repository_path(repo);
    char *const argv[] = {
        "git", "--no-pager", "-C", (char *)path,
        "describe", "--long", "--tags", "HEAD", NULL
    };
    int ret = run_git_capture(repo, out, argv);
    if (ret == 0 && out->l && out->s[out->l - 1] == '\n') {
        out->s[--out->l] = '\0';
    }
    return ret;
}

static int get_branch_upstream_name(git_repository *repo, const char *branch_name, kstring_t *out) {
    git_reference *branch = NULL;
    git_reference *upstream = NULL;
    const char *name = NULL;
    int ret;

    ret = git_branch_lookup(&branch, repo, branch_name, GIT_BRANCH_LOCAL);
    GH_CHECK_RET(ret, "git_branch_lookup(%s)", branch_name);

    ret = git_branch_upstream(&upstream, branch);
    git_reference_free(branch);
    GH_CHECK_RET(ret, "git_branch_upstream(%s)", branch_name);   /* ENOTFOUND: no upstream */

    ret = git_branch_name(&name, upstream);
    if (ret == 0)
        kputs(name, out);
    else
        GH_LOG_GIT_ERR(ret, "git_branch_name(upstream of %s)", branch_name);

    git_reference_free(upstream);
    return ret;
}

static int git_head_subject(git_repository *repo, kstring_t *out, const char *target) {
    git_reference *head = NULL;
    git_object *obj = NULL;
    git_commit *commit = NULL;
    int error;

    error = git_reference_lookup(&head, repo, target);
    if (error != 0) {
        GH_LOG_GIT_ERR(error, "git_reference_lookup(%s)", target);
        return error;
    }

    error = git_reference_peel(&obj, head, GIT_OBJECT_COMMIT);
    git_reference_free(head);

    if (error != 0) {
        GH_LOG_GIT_ERR(error, "git_reference_peel(%s -> commit)", target);
        return error;
    }

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
        GH_LOG_GIT_ERR(error, "git_reference_lookup(HEAD)");
        return error;
    }

    if (git_reference_type(head_ref) != GIT_REFERENCE_SYMBOLIC) {
        GH_LOG_DEBUG("HEAD is not a symbolic ref (detached HEAD)");
        git_reference_free(head_ref);
        return GIT_ENOTFOUND;
    }

    target = git_reference_symbolic_target(head_ref);
    if (!target) {
        GH_LOG_ERROR("HEAD is symbolic but has no target");
        git_reference_free(head_ref);
        return GIT_ERROR;
    }

    error = git_reference_lookup(&resolved_ref, repo, target);
    if (error != 0) {
        const char *shorthand = target;
        if (strncmp(target, "refs/heads/", 11) == 0) {
            shorthand = target + 11;
        }
        GH_LOG_DEBUG("HEAD -> %s does not resolve (unborn branch?), using '%s'",
                     target, shorthand);
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
        GH_LOG_GIT_ERR(error, "git_repository_config");
        return error;
    }

    error = git_config_iterator_new(&iter, cfg);
    if (error != 0) {
        GH_LOG_GIT_ERR(error, "git_config_iterator_new");
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

    if (error != GIT_ITEROVER)
        GH_LOG_GIT_ERR(error, "git_config_next");

    return (error == GIT_ITEROVER) ? 0 : error;
}

static int git_rev_head(git_repository *repo, char result_buf[GIT_OID_MAX_HEXSIZE + 1]) {
    git_reference *head = NULL;
    int error = git_repository_head(&head, repo);
    if (error != 0) {
        GH_LOG_GIT_ERR(error, "git_repository_head");
        return error;
    }
    const git_oid *oid = git_reference_target(head);
    if (oid == NULL) {
        GH_LOG_ERROR("HEAD reference %s has no direct target", git_reference_name(head));
        git_reference_free(head);
        return -1;
    }
    git_oid_tostr(result_buf, GIT_OID_MAX_HEXSIZE + 1, oid);
    git_reference_free(head);
    return 0;
}

/* Task wrapper function implementations - after all functions they call */
static int task_rev_head(git_repository *repo, kstring_t *out, void *arg) {
    char *oid_str = (char *)arg;
    return git_rev_head(repo, oid_str);
}

static int task_config_list_z(git_repository *repo, kstring_t *out, void *arg) {
    return git_config_list_z(repo, out);
}

static int task_head_subject(git_repository *repo, kstring_t *out, void *arg) {
    return git_head_subject(repo, out, "HEAD");
}

static int task_symbolic_ref_short_head(git_repository *repo, kstring_t *out, void *arg) {
    return git_symbolic_ref_short_head(repo, out);
}

static int task_branch_upstream_name(git_repository *repo, kstring_t *out, void *arg) {
    char *branch_name = (char *)arg;
    return get_branch_upstream_name(repo, branch_name, out);
}

static int task_tag_desc(git_repository *repo, kstring_t *out, void *arg) {
    return get_tag_desc(repo, out);
}

static int task_tag_desc_if_match(git_repository *repo, kstring_t *out, void *arg) {
    return get_tag_desc_if_match(repo, out);
}

static int task_worktrees_porcelain(git_repository *repo, kstring_t *out, void *arg) {
    return list_worktrees_porcelain(repo, out);
}

static int task_config_show_untracked(git_repository *repo, kstring_t *out, void *arg) {
    return get_config_status_show_untracked_files(repo, out);
}

static int task_status_porcelain_z(git_repository *repo, kstring_t *out, void *arg) {
    return get_status_porcelain_z(repo, out);
}

static int task_diff_patch(git_repository *repo, kstring_t *out, void *arg) {
    return get_diff_patch(repo, out);
}

static int task_staged(git_repository *repo, kstring_t *out, void *arg) {
    return get_staged(repo, out);
}

static int task_stash_oid(git_repository *repo, kstring_t *out, void *arg) {
    return get_stash_oid(repo, out);
}

static int task_branch_list(git_repository *repo, kstring_t *out, void *arg) {
    return get_branch_list(repo, out);
}

static int task_rev_parse_verify(git_repository *repo, kstring_t *out, void *arg) {
    return get_rev_parse_verify(repo, "HEAD~30", out);
}

static int task_log(git_repository *repo, kstring_t *out, void *arg) {
    return get_log(repo, 30, out);
}

static int task_rev_parse_short_head(git_repository *repo, kstring_t *out, void *arg) {
    return get_rev_parse_short_head(repo, out);
}

static int task_tag_master(git_repository *repo, kstring_t *out, void *arg) {
    return get_tag_master_oid(repo, out);
}

static int task_tag_origin_master(git_repository *repo, kstring_t *out, void *arg) {
    return get_ref_oid(repo, "refs/tags/origin/master", out);
}

static int task_tag_origin_head(git_repository *repo, kstring_t *out, void *arg) {
    return get_ref_oid(repo, "refs/tags/origin/HEAD", out);
}

static int task_origin_head(git_repository *repo, kstring_t *out, void *arg) {
    return get_origin_head(repo, out);
}

static int task_upstream_branch_master(git_repository *repo, kstring_t *out, void *arg) {
    return get_upstream_branch(repo, "master", out);
}

static int task_upstream_subj(git_repository *repo, kstring_t *out, void *arg) {
    char *target = (char *)arg;
    return git_head_subject(repo, out, target);
}

#define RUN_STATUS_TASK_IMPL(name, task, out, arg, optional)             \
    ({                                                                  \
        uint64_t t0_ = gh_now_ms();                                      \
        git_error_clear();                                              \
        int ret_ = task(g_repo, out, arg);                              \
        gh_log_step(__func__, __LINE__, name, ret_,                    \
                    gh_now_ms() - t0_, optional);                      \
        ret_;                                                           \
    })
#define RUN_STATUS_TASK(name, task, out, arg) \
    RUN_STATUS_TASK_IMPL(name, task, out, arg, 0)
#define RUN_STATUS_TASK_OPT(name, task, out, arg) \
    RUN_STATUS_TASK_IMPL(name, task, out, arg, 1)

static void git_root_worker(uv_work_t *req) {
    git_root_req_T *data = req->data;
    const uint64_t id = data->id;
    const uint64_t t_start = gh_now_ms();
    git_repository *g_repo = NULL;

    GH_LOG_DEBUG("request %" PRIu64 ": worker started", id);

    int open_ret = git_repository_open_ext(&g_repo, data->repo_path->s, 0, NULL);
    if (open_ret < 0) {
        GH_LOG_GIT_ERR_AT(GH_LOG_LEVEL_ERROR, open_ret,
                          "git_repository_open_ext(%s)", data->repo_path->s);
        return;
    }
    data->repo = g_repo;

    const char *root = git_repository_workdir(g_repo);
    char oid_str[GIT_OID_MAX_HEXSIZE + 1];

    /* Phase 1: Independent tasks - run directly (we're already in a worker thread) */
    data->list_z = str_create(NULL, 0);
    int list_z_ret = RUN_STATUS_TASK("config_list_z", task_config_list_z, data->list_z, NULL);

    data->head_log_line = str_create(NULL, 0);
    data->subj = str_create(NULL, 0);
    int subj_ret = RUN_STATUS_TASK("head_subject", task_head_subject, data->subj, NULL);

    int rev_ret = RUN_STATUS_TASK("rev_head", task_rev_head, NULL, oid_str);

    data->tag_desc = str_create(NULL, 0);
    int tag_desc_ret = RUN_STATUS_TASK_OPT("tag_desc", task_tag_desc, data->tag_desc, NULL);

    data->opt_tag_desc_head = str_create(NULL, 0);
    int opt_tag_desc_head_ret = RUN_STATUS_TASK("tag_desc_if_match", task_tag_desc_if_match, data->opt_tag_desc_head, NULL);

    data->worktree_porcelain = str_create(NULL, 0);
    int worktree_porcelain_ret = RUN_STATUS_TASK("worktrees_porcelain", task_worktrees_porcelain, data->worktree_porcelain, NULL);

    data->config_status_show_untracked_files = str_create(NULL, 0);
    int config_status_show_untracked_files_ret = RUN_STATUS_TASK("config_show_untracked_files", task_config_show_untracked, data->config_status_show_untracked_files, NULL);

    data->status = str_create(NULL, 0);
    int status_ret = RUN_STATUS_TASK("status_porcelain_z", task_status_porcelain_z, data->status, NULL);

    data->diff = str_create(NULL, 0);
    int diff_ret = RUN_STATUS_TASK("diff_patch", task_diff_patch, data->diff, NULL);

    data->staged = str_create(NULL, 0);
    int staged_ret = RUN_STATUS_TASK("staged", task_staged, data->staged, NULL);

    data->stash = str_create(NULL, 0);
    int stash_ret = RUN_STATUS_TASK_OPT("stash_oid", task_stash_oid, data->stash, NULL);

    data->branches = str_create(NULL, 0);
    int branches_ret = RUN_STATUS_TASK("branch_list", task_branch_list, data->branches, NULL);

    data->revision_by_idx = str_create(NULL, 0);
    int revision_ret = RUN_STATUS_TASK_OPT("rev_parse_verify", task_rev_parse_verify, data->revision_by_idx, NULL);

    data->logs = str_create(NULL, 0);
    int logs_ret = RUN_STATUS_TASK("log", task_log, data->logs, NULL);

    data->rev_short_head = str_create(NULL, 0);
    int rev_short_head = RUN_STATUS_TASK("rev_parse_short_head", task_rev_parse_short_head, data->rev_short_head, NULL);

    data->tag_master = str_create(NULL, 0);
    int tag_master = RUN_STATUS_TASK_OPT("tag_master", task_tag_master, data->tag_master, NULL);

    data->tag_origin_master = str_create(NULL, 0);
    int tag_origin_master_ret = RUN_STATUS_TASK_OPT("tag_origin_master", task_tag_origin_master, data->tag_origin_master, NULL);

    data->tag_origin_head = str_create(NULL, 0);
    int tag_origin_head_ret = RUN_STATUS_TASK_OPT("tag_origin_head", task_tag_origin_head, data->tag_origin_head, NULL);

    data->origin_head = str_create(NULL, 0);
    int origin_head_ret = RUN_STATUS_TASK_OPT("origin_head", task_origin_head, data->origin_head, NULL);

    data->upstream_branch_master = str_create(NULL, 0);
    int upstream_branch_master_ret = RUN_STATUS_TASK_OPT("upstream_branch_master", task_upstream_branch_master, data->upstream_branch_master, NULL);

    data->is_bare = git_repository_is_bare(g_repo) ? 1 : 0;

    /* Phase 2: Tasks that depend on branch */
    data->branch = str_create(NULL, 0);
    int branch_ret = RUN_STATUS_TASK_OPT("symbolic_ref_short_head", task_symbolic_ref_short_head, data->branch, NULL);
    if (branch_ret != 0)
        GH_LOG_DEBUG("request %" PRIu64 ": no current branch (detached HEAD?) rc=%d, "
                     "branch left empty", id, branch_ret);

    /* Phase 3: Tasks that depend on upstream_branch */
    data->upstream_branch = str_create(NULL, 0);
    int upstream_branch_ret = -1;
    if (branch_ret == 0) {
        upstream_branch_ret = RUN_STATUS_TASK_OPT("branch_upstream_name", task_branch_upstream_name, data->upstream_branch, data->branch->s);
        if (upstream_branch_ret != 0)
            GH_LOG_DEBUG("request %" PRIu64 ": branch '%s' has no upstream rc=%d",
                         id, data->branch->s, upstream_branch_ret);
    } else {
        GH_LOG_DEBUG("request %" PRIu64 ": skipping upstream lookup, no current branch", id);
    }

    /* Phase 4: Tasks that depend on upstream_subj */
    data->upstream_subj = str_create(NULL, 0);
    int upstream_subj_ret = -1;
    if (upstream_branch_ret == 0) {
        char target_str[] = "refs/remotes/";
        STR_CLEANUP kstring_t *target = str_create(target_str, sizeof(target_str) - 1);
        kputs(data->upstream_branch->s, target);
        upstream_subj_ret = RUN_STATUS_TASK_OPT("upstream_subj", task_upstream_subj, data->upstream_subj, target->s);
    }

    if (tag_desc_ret != 0) {
        /* `git describe --tags` fails when no tag is reachable. That is not an
         * error for status; tag_desc just stays empty. */
        GH_LOG_DEBUG("request %" PRIu64 ": no tag description (no tags reachable "
                     "from HEAD?) rc=%d, continuing", id, tag_desc_ret);
    }

    if (root && rev_ret == 0 && list_z_ret == 0 && subj_ret == 0 && opt_tag_desc_head_ret == 0 && worktree_porcelain_ret == 0 && config_status_show_untracked_files_ret == 0 && status_ret == 0) {
        data->root = str_create(root, strlen(root) - 1); // trim last slash
        data->rev_head = str_create(oid_str, strlen(oid_str));
        kputs(data->rev_head->s, data->head_log_line);
        kputs(" ", data->head_log_line);
        kputs(data->subj->s, data->head_log_line);
        data->version = git_version_create();
        data->result = 0;
    } else {
        GH_LOG_ERROR("request %" PRIu64 ": incomplete result, no response will be sent: "
                     "root=%s rev_head=%d config_list=%d head_subject=%d "
                     "tag_desc_head=%d worktrees=%d show_untracked=%d status=%d",
                     id, root ? "ok" : "NULL (bare repo?)", rev_ret, list_z_ret, subj_ret,
                     opt_tag_desc_head_ret, worktree_porcelain_ret,
                     config_status_show_untracked_files_ret, status_ret);
    }

    /* Non-gating steps: report failures that are not the expected ones. */
    if (diff_ret != 0 || staged_ret != 0 || branches_ret != 0 || logs_ret != 0)
        GH_LOG_WARN("request %" PRIu64 ": non-fatal step failures: diff=%d staged=%d "
                    "branches=%d log=%d", id, diff_ret, staged_ret, branches_ret, logs_ret);

    GH_LOG_DEBUG("request %" PRIu64 ": worker finished, result=%d, total %" PRIu64 " ms",
                 id, data->result, gh_now_ms() - t_start);
}

static void after_git_root(uv_work_t *req, int status) {
    GIT_ROOT_REQUEST_CLEANUP git_root_req_T *data = req->data;
    if (status != 0) {
        GH_LOG_ERROR("request %" PRIu64 ": work did not complete: %s (%d)",
                     data->id, uv_strerror(status), status);
        return;
    }
    GH_LOG_DEBUG("request %" PRIu64 ": after_git_root called, result=%d", data->id, data->result);
    if (data->result < 0) {
        GH_LOG_WARN("request %" PRIu64 ": worker reported failure, dropping request "
                    "(no response sent)", data->id);
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
        .dot_git_dir = data->dot_git_dir,
        .config_status_show_untracked_files = data->config_status_show_untracked_files,
        .status = data->status,
        .diff = data->diff,
        .staged = data->staged,
        .is_bare = data->is_bare,
        .stash = data->stash,
        .branches = data->branches,
        .revision_by_idx = data->revision_by_idx,
        .logs = data->logs,
        .rev_short_head = data->rev_short_head,
        .tag_master = data->tag_master,
        .tag_origin_master = data->tag_origin_master,
        .origin_head = data->origin_head,
        .tag_origin_head = data->tag_origin_head,
        .upstream_branch = data->upstream_branch,
    };
    GH_LOG_DEBUG("request %" PRIu64 ": sending response", data->id);
    msgpack_handler_send(&res);
}

int git_handler_queue_git_status(uv_loop_t *loop, u_int64_t id, kstring_t *pwd) {
    /* calloc: the worker may return early, and cleanup must only ever see
     * NULL or valid pointers in the fields it did not get to. */
    GIT_ROOT_REQUEST_CLEANUP git_root_req_T *data = calloc(1, sizeof(*data));
    if (!data) {
        GH_LOG_ERROR("request %" PRIu64 ": calloc(%zu) failed", (uint64_t)id, sizeof(*data));
        return -1;
    }
    data->pwd = pwd;
    data->id = id;
    data->result = -1;
    if (!g_repo) {
        GH_LOG_ERROR("request %" PRIu64 ": no repository is open "
                     "(git_handler_repo_init not called or failed)", (uint64_t)id);
        return -1;
    }
    const char *repo_path = git_repository_workdir(g_repo);
    if (!repo_path)
        repo_path = git_repository_path(g_repo);
    if (!repo_path) {
        GH_LOG_ERROR("request %" PRIu64 ": repository has no usable path", (uint64_t)id);
        return -1;
    }
    data->repo_path = str_create(repo_path, strlen(repo_path));
    if (g_dot_git_dir)
        data->dot_git_dir = str_create(g_dot_git_dir->s, g_dot_git_dir->l);
    data->req.data = data;
    GH_LOG_DEBUG("request %" PRIu64 ": queueing git status for pwd=%s", (uint64_t)id,
                 (pwd && pwd->s) ? pwd->s : "(null)");
    uv_work_t *req = &data->req;
    int ret = uv_queue_work(loop, req, git_root_worker, after_git_root);
    if (ret == 0) {
        xfer(data); // until after_git_root
    } else {
        GH_LOG_ERROR("request %" PRIu64 ": uv_queue_work failed: %s (%d)", (uint64_t)id,
                     uv_strerror(ret), ret);
    }
    return ret;
}

int git_handler_repo_init(const char *path) {
    git_buf gitdir = GIT_BUF_INIT;
    kstring_t *out = str_create(NULL, 0);
    int ret;

    GH_LOG_INFO("opening repository at %s", path ? path : "(null)");

    ret = git_repository_discover(&gitdir, path, 0, NULL);
    if (ret == 0) {
        kputs(gitdir.ptr, out);
        g_dot_git_dir = out;
        git_buf_dispose(&gitdir);
        GH_LOG_DEBUG("discovered git dir %s", g_dot_git_dir->s);
    } else {
        GH_LOG_GIT_ERR_AT(GH_LOG_LEVEL_WARN, ret, "git_repository_discover(%s)",
                          path ? path : "(null)");
    }

    ret = git_repository_open_ext(&g_repo, path, 0, NULL);
    if (ret < 0) {
        GH_LOG_GIT_ERR_AT(GH_LOG_LEVEL_ERROR, ret, "git_repository_open_ext(%s)",
                          path ? path : "(null)");
        return ret;
    }

    GH_LOG_INFO("repository opened: workdir=%s bare=%d",
                git_repository_workdir(g_repo) ? git_repository_workdir(g_repo) : "(none)",
                git_repository_is_bare(g_repo));
    return ret;
}

void git_handler_repo_deinit() {
    GH_LOG_INFO("closing repository");
    git_repository_free(g_repo);
    if (g_dot_git_dir) {
        free(g_dot_git_dir->s);
        free(g_dot_git_dir);
    }
}
