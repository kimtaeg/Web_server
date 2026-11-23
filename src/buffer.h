#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
int buf_reserve(buf_t *b, size_t extra);
int buf_append(buf_t *b, const void *src, size_t n);
int buf_append_str(buf_t *b, const char *s);
int buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_consume(buf_t *b, size_t n);
void buf_clear(buf_t *b);

#endif
