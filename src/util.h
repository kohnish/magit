#include <kstring.h>

#define xfer(x)                                                         \
    ({                                                                                                                                                                                                                                                                                                     \
        __auto_type _tmp = (x);                                                                                                                                                                                                                                                                            \
        (x) = NULL;                                                                                                                                                                                                                                                                                        \
        _tmp;                                                                                                                                                                                                                                                                                              \
    })


void str_cleanup(kstring_t **p);
#define STR_CLEANUP __attribute__((cleanup(str_cleanup)))
kstring_t *str_create(const char *data, int len);
kstring_t *str_cat_xfer(kstring_t *s1, kstring_t *s2, const char *delimiter);

void free_ptr(void *p);
#define PTR_CLEANUP __attribute__((cleanup(free_ptr)))
