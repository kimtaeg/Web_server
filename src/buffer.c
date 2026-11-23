#include "buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b) {
    free(b->data);
    buf_init(b);
}

int buf_reserve(buf_t *b, size_t extra) {
    if (extra > (size_t)-1 - b->len - 1)
        return -1;
    size_t need = b->len + extra + 1; /* always keep room for a NUL terminator */
    if (need <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need)
        ncap = ncap > ((size_t)-1) / 2 ? need : ncap * 2;
    char *p = realloc(b->data, ncap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = ncap;
    return 0;
}

int buf_append(buf_t *b, const void *src, size_t n) {
    if (buf_reserve(b, n) < 0)
        return -1;
    if (n)
        memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int buf_append_str(buf_t *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

int buf_printf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || buf_reserve(b, (size_t)n) < 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

void buf_consume(buf_t *b, size_t n) {
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
    if (b->data)
        b->data[b->len] = '\0';
}

void buf_clear(buf_t *b) {
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}
