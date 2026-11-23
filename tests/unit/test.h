#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

extern int t_checks, t_failures;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        t_checks++;                                                                       \
        if (!(cond)) {                                                                    \
            t_failures++;                                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                                 \
    } while (0)

#define CHECK_INT(a, b)                                                                   \
    do {                                                                                  \
        long long a_ = (long long)(a), b_ = (long long)(b);                               \
        t_checks++;                                                                       \
        if (a_ != b_) {                                                                   \
            t_failures++;                                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,        \
                    __LINE__, #a, a_, b_);                                                \
        }                                                                                 \
    } while (0)

#define CHECK_STR(a, b)                                                                   \
    do {                                                                                  \
        const char *a_ = (a), *b_ = (b);                                                  \
        t_checks++;                                                                       \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                          \
            t_failures++;                                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__,    \
                    __LINE__, #a, a_ ? a_ : "(null)", b_ ? b_ : "(null)");                \
        }                                                                                 \
    } while (0)

void suite_http(void);
void suite_mime(void);
void suite_router(void);
void suite_cache(void);
void suite_config(void);
void suite_static(void);
void suite_proxy(void);
void suite_cgi(void);
void suite_threadpool(void);

#endif
