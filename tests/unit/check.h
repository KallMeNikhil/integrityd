#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <string.h>

static int check_count, check_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        check_count++;                                                           \
        if (!(cond)) {                                                           \
            check_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_STREQ(a, b)                                                        \
    do {                                                                         \
        const char *a_ = (a), *b_ = (b);                                         \
        check_count++;                                                           \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                 \
            check_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK_STREQ failed: \"%s\" != \"%s\"\n",     \
                    __FILE__, __LINE__, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
        }                                                                        \
    } while (0)

#define CHECK_DONE()                                                             \
    (printf("%-28s %4d checks, %d failed\n", __FILE__, check_count, check_failures), \
     check_failures ? 1 : 0)

#endif
