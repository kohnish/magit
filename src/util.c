#include "util.h"

void str_cleanup(kstring_t **p) {
    if (p && *p) {
        free((*p)->s);
        free(*p);
    }
}

kstring_t *str_create(const char *data, int len) {
    kstring_t *s = calloc(1, sizeof(*s));
    kputsn(data, len, s);
    return s;
}

void free_ptr(void *p) {
    void **ptr = p;

    if (*ptr)
        free(*ptr);
}
