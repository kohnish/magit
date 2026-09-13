#include "util.h"

void str_cleanup(kstring_t **p) {
    if (p && *p) {
        free((*p)->s);
        free(*p);
    }
}

kstring_t *str_create(const char *data, int len) {
    kstring_t *s = calloc(1, sizeof(*s));
    if (data) {
        kputsn(data, len, s);
    }
    return s;
}

kstring_t *str_cat_xfer(kstring_t *s1, kstring_t *s2, const char *delimiter) {
    if (delimiter)
        kputs(delimiter, s1);

    kputsn(s2->s, s2->l, s1);

    free(s2->s);
    free(s2);

    return s1;
}

void free_ptr(void *p) {
    void **ptr = p;

    if (*ptr)
        free(*ptr);
}
