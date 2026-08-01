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

void free_ptr(void *p);
#define PTR_CLEANUP __attribute__((cleanup(free_ptr)))
