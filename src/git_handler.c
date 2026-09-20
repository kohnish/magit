#include "kstring.h"
#include "msgpack_handler.h"
#include "util.h"
#include <errno.h>
#include <git2.h>
#include <git2/sys/errors.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
#include "kvec.h"

#define FF '\x0c'
#define CLEANUP(fn) __attribute__((cleanup(fn)))

#define DEFINE_CLEANUP(type, free_fn)                        \
    static inline void cleanup_##type(type **p) {            \
        if (*p)                                              \
            free_fn(*p);                                     \
    }

DEFINE_CLEANUP(git_revwalk,            git_revwalk_free)
DEFINE_CLEANUP(git_mailmap,            git_mailmap_free)
DEFINE_CLEANUP(git_reference,          git_reference_free)
DEFINE_CLEANUP(git_reference_iterator, git_reference_iterator_free)
DEFINE_CLEANUP(git_object,             git_object_free)
DEFINE_CLEANUP(git_signature,          git_signature_free)

static inline void cleanup_git_buf(git_buf *b) { git_buf_dispose(b); }

/* ---- commit list: malloc'd arrays plus per-commit decoration names ---- */

typedef kvec_t(char *) namevec_t;

typedef struct {
    git_commit **commits;   /* calloc'd, `count` slots filled */
    namevec_t   *decor;     /* calloc'd, one vector per slot  */
    size_t       count;
    size_t       cap;
} commit_list_t;

static int commit_list_init(commit_list_t *l, size_t cap) {
    l->count = 0;
    l->cap = cap;
    l->commits = calloc(cap, sizeof(*l->commits));
    l->decor = calloc(cap, sizeof(*l->decor));
    return (l->commits && l->decor) ? 0 : -1;
}

static void commit_list_free(commit_list_t *l) {
    for (size_t i = 0; i < l->count; i++) {
        for (size_t j = 0; j < kv_size(l->decor[i]); j++)
            free(kv_A(l->decor[i], j));
        kv_destroy(l->decor[i]);
        git_commit_free(l->commits[i]);
    }
    free(l->decor);
    free(l->commits);
}

static void put_i64(kstring_t *out, long long n) {
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%lld", n);
    if (len > 0)
        kputsn(tmp, (size_t)len, out);
}

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

/* git log --format=%h%x0c%D%x0c%x0c%aN%x0c%at%x0c%s --decorate=full
 *         -n<limit> --use-mailmap --no-prefix --                      */
static int get_log(git_repository *repo, size_t limit, kstring_t *out) {
    CLEANUP(commit_list_free)             commit_list_t list = {0};
    CLEANUP(cleanup_git_revwalk)          git_revwalk *walk = NULL;
    CLEANUP(cleanup_git_mailmap)          git_mailmap *mailmap = NULL;
    CLEANUP(cleanup_git_reference)        git_reference *head = NULL;
    CLEANUP(cleanup_git_reference_iterator) git_reference_iterator *iter = NULL;
    const char *head_name;
    const git_oid *head_oid;
    int head_attached;
    git_oid oid;
    int ret;

    if (limit == 0)
        return 0;
    if (commit_list_init(&list, limit) < 0) {
        GH_LOG_ERROR("commit_list_init(%zu) failed: out of memory", limit);
        return -1;
    }

    /* 1. Walk HEAD, newest first, at most `limit` commits. */
    ret = git_revwalk_new(&walk, repo);
    GH_CHECK_RET(ret, "git_revwalk_new");
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    ret = git_revwalk_push_head(walk);          /* fails on unborn HEAD */
    GH_CHECK_RET(ret, "git_revwalk_push_head");

    while (list.count < limit && (ret = git_revwalk_next(&oid, walk)) == 0) {
        ret = git_commit_lookup(&list.commits[list.count], repo, &oid);
        GH_CHECK_RET(ret, "git_commit_lookup (commit #%zu)", list.count);
        list.count++;
    }
    if (ret < 0 && ret != GIT_ITEROVER) {
        GH_LOG_GIT_ERR(ret, "git_revwalk_next");
        return ret;
    }

    ret = git_mailmap_from_repository(&mailmap, repo);
    GH_CHECK_RET(ret, "git_mailmap_from_repository");

    /* 2. HEAD: attached to a branch, or detached. */
    ret = git_repository_head(&head, repo);
    GH_CHECK_RET(ret, "git_repository_head");
    head_name = git_reference_name(head);       /* "refs/heads/x" or "HEAD" */
    head_attached = git_reference_is_branch(head);
    head_oid = git_reference_target(head);

    /* 3. Decorations: every ref whose peeled commit is in our list. */
    ret = git_reference_iterator_new(&iter, repo);
    GH_CHECK_RET(ret, "git_reference_iterator_new");

    for (;;) {
        CLEANUP(cleanup_git_reference) git_reference *ref = NULL;
        CLEANUP(cleanup_git_object)    git_object *obj = NULL;
        const char *name;

        ret = git_reference_next(&ref, iter);
        if (ret == GIT_ITEROVER)
            break;
        GH_CHECK_RET(ret, "git_reference_next");

        name = git_reference_name(ref);
        if (strncmp(name, "refs/prefetch/", 14) == 0 ||
            (head_attached && strcmp(name, head_name) == 0))
            continue;   /* current branch is printed via "HEAD -> ..." */

        /* Peels annotated tags and symbolic refs (origin/HEAD) to a commit. */
        if (git_reference_peel(&obj, ref, GIT_OBJECT_COMMIT) < 0) {
            GH_LOG_DEBUG("skipping decoration for %s: cannot peel to a commit", name);
            git_error_clear();
            continue;
        }

        for (size_t i = 0; i < list.count; i++) {
            if (!git_oid_equal(git_object_id(obj), git_commit_id(list.commits[i])))
                continue;

            int is_tag = strncmp(name, "refs/tags/", 10) == 0;
            size_t len = strlen(name) + 6;      /* "tag: " + name + NUL */
            char *s = malloc(len);
            if (!s) {
                GH_LOG_ERROR("malloc(%zu) failed for decoration of %s", len, name);
                return -1;
            }
            snprintf(s, len, "%s%s", is_tag ? "tag: " : "", name);
            kv_push(char *, list.decor[i], s);
            break;
        }
    }

    /* 4. Emit one line per commit. */
    for (size_t i = 0; i < list.count; i++) {
        CLEANUP(cleanup_git_buf)       git_buf sid = GIT_BUF_INIT;
        CLEANUP(cleanup_git_signature) git_signature *sig = NULL;
        git_commit *c = list.commits[i];
        const char *summary;
        int first = 1;

        /* %h */
        ret = git_object_short_id(&sid, (git_object *)c);
        GH_CHECK_RET(ret, "git_object_short_id (commit #%zu)", i);
        kputs(sid.ptr, out);
        kputc(FF, out);

        /* %D: HEAD entry first, then the rest in reverse ref order. */
        if (git_oid_equal(head_oid, git_commit_id(c))) {
            if (head_attached) {
                kputs("HEAD -> ", out);
                kputs(head_name, out);
            } else {
                kputs("HEAD", out);
            }
            first = 0;
        }
        for (size_t j = kv_size(list.decor[i]); j > 0; j--) {
            if (!first)
                kputs(", ", out);
            kputs(kv_A(list.decor[i], j - 1), out);
            first = 0;
        }
        kputc(FF, out);
        kputc(FF, out);                         /* the empty field */

        /* %aN, %at */
        ret = git_commit_author_with_mailmap(&sig, c, mailmap);
        GH_CHECK_RET(ret, "git_commit_author_with_mailmap (commit #%zu)", i);
        kputs(sig->name, out);
        kputc(FF, out);
        put_i64(out, (long long)sig->when.time);
        kputc(FF, out);

        /* %s */
        summary = git_commit_summary(c);
        kputs(summary ? summary : "", out);
        kputc('\n', out);
    }

    GH_LOG_DEBUG("emitted %zu log entries (limit %zu)", list.count, limit);
    return 0;
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
    git_object *head = NULL;
    git_describe_result *result = NULL;
    git_describe_options opts = GIT_DESCRIBE_OPTIONS_INIT;
    git_describe_format_options fmt = GIT_DESCRIBE_FORMAT_OPTIONS_INIT;
    git_buf buf = GIT_BUF_INIT;
    int ret;

    ret = git_revparse_single(&head, repo, "HEAD");
    GH_CHECK_RET(ret, "git_revparse_single(HEAD)");

    opts.describe_strategy = GIT_DESCRIBE_TAGS;

    ret = git_describe_commit(&result, head, &opts);
    git_object_free(head);

    /* With no reachable tag libgit2 returns GIT_ERROR (-1), not ENOTFOUND.
     * That is an expected outcome, so it is logged at DEBUG. */
    if (ret < 0) {
        GH_LOG_GIT_ERR_AT(GH_LOG_LEVEL_DEBUG, ret, "git_describe_commit");
        return ret;
    }

    fmt.always_use_long_format = 1;

    ret = git_describe_format(&buf, result, &fmt);
    if (ret == 0)
        kputs(buf.ptr, out);
    else
        GH_LOG_GIT_ERR(ret, "git_describe_format");

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

static void git_root_worker(uv_work_t *req) {
    git_root_req_T *data = req->data;
    const uint64_t id = data->id;
    const uint64_t t_start = gh_now_ms();

    GH_LOG_DEBUG("request %" PRIu64 ": worker started", id);

    if (!g_repo) {
        GH_LOG_ERROR("request %" PRIu64 ": no repository is open "
                     "(git_handler_repo_init not called or failed)", id);
        return;
    }

    const char *root = git_repository_workdir(g_repo);
    char oid_str[GIT_OID_MAX_HEXSIZE + 1];   /* +1 for the NUL git_oid_tostr writes */
    int rev_ret;
    GH_STEP(rev_ret, "rev_head", git_rev_head(g_repo, oid_str));

    data->list_z = str_create(NULL, 0);
    int list_z_ret;
    GH_STEP(list_z_ret, "config_list_z", git_config_list_z(g_repo, data->list_z));

    data->head_log_line = str_create(NULL, 0);
    data->subj = str_create(NULL, 0);
    int subj_ret;
    GH_STEP(subj_ret, "head_subject", git_head_subject(g_repo, data->subj, "HEAD"));

    /* Detached HEAD and branches without an upstream are ordinary states, not
     * failures: the corresponding fields simply stay empty and the response is
     * still sent. */
    data->branch = str_create(NULL, 0);
    int branch_ret;
    GH_STEP_OPT(branch_ret, "symbolic_ref_short_head",
                git_symbolic_ref_short_head(g_repo, data->branch));
    if (branch_ret != 0)
        GH_LOG_DEBUG("request %" PRIu64 ": no current branch (detached HEAD?) rc=%d, "
                     "branch left empty", id, branch_ret);

    data->upstream_branch = str_create(NULL, 0);
    int upstream_branch_ret = -1;   /* stays -1 when the step is skipped */
    if (branch_ret == 0) {
        GH_STEP_OPT(upstream_branch_ret, "branch_upstream_name",
                    get_branch_upstream_name(g_repo, data->branch->s, data->upstream_branch));
        if (upstream_branch_ret != 0)
            GH_LOG_DEBUG("request %" PRIu64 ": branch '%s' has no upstream rc=%d",
                         id, data->branch->s, upstream_branch_ret);
    } else {
        GH_LOG_DEBUG("request %" PRIu64 ": skipping upstream lookup, no current branch", id);
    }

    data->tag_desc = str_create(NULL, 0);
    int tag_desc_ret;
    GH_STEP_OPT(tag_desc_ret, "tag_desc", get_tag_desc(g_repo, data->tag_desc));
    if (tag_desc_ret != 0) {
        /* `git describe --tags` fails when no tag is reachable. That is not an
         * error for status; tag_desc just stays empty. */
        GH_LOG_DEBUG("request %" PRIu64 ": no tag description (no tags reachable "
                     "from HEAD?) rc=%d, continuing", id, tag_desc_ret);
    }

    data->upstream_subj = str_create(NULL, 0);
    int upstream_subj_ret = -1;     /* stays -1 when the step is skipped */
    if (upstream_branch_ret == 0) {
        char target_str[] = "refs/remotes/";
        STR_CLEANUP kstring_t *target = str_create(target_str, sizeof(target_str) - 1);
        kputs(data->upstream_branch->s, target);
        /* Optional: fails e.g. when the upstream is a local branch, where
         * "refs/remotes/<name>" does not exist. The subject just stays empty. */
        GH_STEP_OPT(upstream_subj_ret, "upstream_subj",
                    git_head_subject(g_repo, data->upstream_subj, target->s));
    }

    data->opt_tag_desc_head = str_create(NULL, 0);
    int opt_tag_desc_head_ret;
    GH_STEP(opt_tag_desc_head_ret, "tag_desc_if_match",
            get_tag_desc_if_match(g_repo, data->opt_tag_desc_head));

    data->worktree_porcelain = str_create(NULL, 0);
    int worktree_porcelain_ret;
    GH_STEP(worktree_porcelain_ret, "worktrees_porcelain",
            list_worktrees_porcelain(g_repo, data->worktree_porcelain));

    data->config_status_show_untracked_files = str_create(NULL, 0);
    int config_status_show_untracked_files_ret;
    GH_STEP(config_status_show_untracked_files_ret, "config_show_untracked_files",
            get_config_status_show_untracked_files(g_repo, data->config_status_show_untracked_files));

    data->status = str_create(NULL, 0);
    int status_ret;
    GH_STEP(status_ret, "status_porcelain_z", get_status_porcelain_z(g_repo, data->status));

    data->diff = str_create(NULL, 0);
    int diff_ret;
    GH_STEP(diff_ret, "diff_patch", get_diff_patch(g_repo, data->diff));

    data->staged = str_create(NULL, 0);
    int staged_ret;
    GH_STEP(staged_ret, "staged", get_staged(g_repo, data->staged));

    data->is_bare = git_repository_is_bare(g_repo) ? 1 : 0;

    /* The remaining steps do not gate the response; a failure here just
     * means the field stays empty (no stash, history shorter than 30, ...). */
    data->stash = str_create(NULL, 0);
    int stash_ret;
    GH_STEP_OPT(stash_ret, "stash_oid", get_stash_oid(g_repo, data->stash));

    data->branches = str_create(NULL, 0);
    int branches_ret;
    GH_STEP(branches_ret, "branch_list", get_branch_list(g_repo, data->branches));

    data->revision_by_idx = str_create(NULL, 0);
    int revision_ret;
    GH_STEP_OPT(revision_ret, "rev_parse_HEAD~30",
                get_rev_parse_verify(g_repo, "HEAD~30", data->revision_by_idx));

    data->logs = str_create(NULL, 0);
    int logs_ret;
    GH_STEP(logs_ret, "log", get_log(g_repo, 30, data->logs));


    data->rev_short_head = str_create(NULL, 0);
    int rev_short_head;
    GH_STEP(rev_short_head, "rev", get_rev_parse_short_head(g_repo, data->rev_short_head));

    data->tag_master = str_create(NULL, 0);
    int tag_master;
    GH_STEP(tag_master, "tag_master", get_tag_master_oid(g_repo, data->tag_master));

    data->tag_origin_master = str_create(NULL, 0);
    get_ref_oid(g_repo, "refs/tags/origin/master", data->tag_origin_master);

    data->origin_head = str_create(NULL, 0);
    get_origin_head(g_repo, data->origin_head);

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
        .dot_git_dir = g_dot_git_dir, // global
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
