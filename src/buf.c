/*
 * buf.c — 상한이 있는 가변 바이트 버퍼. 설계 이유는 buf.h 참고.
 */
#include "buf.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_MIN_CAP 64

/*
 * len 이 need_len 이 될 수 있도록 cap 을 확보한다 (NUL 자리 +1 포함).
 * 2배씩 늘려 append 의 분할 상환 비용을 O(1) 로 만들되, max+1 을 넘겨 할당하지는 않는다.
 * 실패해도 기존 data 는 그대로 둔다 (realloc 실패 시 원본이 유효하므로).
 */
static int buf_reserve(buf_t *b, size_t need_len)
{
    size_t need_cap;
    size_t new_cap;
    char *p;

    if (need_len > b->max)
        return BUF_ERR_LIMIT;
    if (need_len == SIZE_MAX)
        return BUF_ERR_NOMEM; /* +1 하면 넘친다 */
    need_cap = need_len + 1;
    if (need_cap <= b->cap)
        return BUF_OK;

    new_cap = b->cap ? b->cap : BUF_MIN_CAP;
    while (new_cap < need_cap) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = need_cap;
            break;
        }
        new_cap *= 2;
    }
    /* max 보다 큰 공간은 쓸 일이 없다. (max < SIZE_MAX 는 위 검사로 보장) */
    if (b->max < SIZE_MAX && new_cap > b->max + 1)
        new_cap = b->max + 1;

    p = realloc(b->data, new_cap);
    if (p == NULL)
        return BUF_ERR_NOMEM;
    if (b->data == NULL)
        p[0] = '\0';
    b->data = p;
    b->cap = new_cap;
    return BUF_OK;
}

int buf_init(buf_t *b, size_t init_cap, size_t max_cap)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->max = max_cap;

    if (max_cap == 0)
        return BUF_ERR_LIMIT;
    if (init_cap > max_cap)
        init_cap = max_cap;
    if (init_cap == 0)
        return BUF_OK;
    return buf_reserve(b, init_cap);
}

void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int buf_append(buf_t *b, const void *p, size_t n)
{
    int rc;

    if (n == 0)
        return BUF_OK;
    /* len + n 의 overflow 를 피하려고 뺄셈으로 비교한다 (len <= max 불변식). */
    if (n > b->max - b->len)
        return BUF_ERR_LIMIT;
    rc = buf_reserve(b, b->len + n);
    if (rc != BUF_OK)
        return rc;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return BUF_OK;
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int need;
    int written;
    int rc = BUF_OK;

    /* 1차: 필요한 길이만 계산. 2차: 확보한 공간에 실제로 쓴다. */
    va_start(ap, fmt);
    va_copy(ap2, ap);
    need = vsnprintf(NULL, 0, fmt, ap);
    if (need < 0) {
        rc = BUF_ERR_FMT;
        goto cleanup;
    }
    if ((size_t)need > b->max - b->len) {
        rc = BUF_ERR_LIMIT;
        goto cleanup;
    }
    rc = buf_reserve(b, b->len + (size_t)need);
    if (rc != BUF_OK)
        goto cleanup;
    written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    if (written != need) {
        b->data[b->len] = '\0'; /* 실패 시 내용 불변 */
        rc = BUF_ERR_FMT;
        goto cleanup;
    }
    b->len += (size_t)need;

cleanup:
    va_end(ap2);
    va_end(ap);
    return rc;
}

/*
 * memmove 로 남은 바이트를 앞으로 당긴다.
 * 읽기 오프셋을 따로 두는 링버퍼보다 단순하고, 남는 양은 보통 파이프라이닝된
 * 다음 요청 일부(수백 바이트)라 복사 비용이 작다.
 */
void buf_consume(buf_t *b, size_t n)
{
    if (b->data == NULL)
        return;
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
    b->data[b->len] = '\0';
}

void buf_reset(buf_t *b)
{
    b->len = 0;
    if (b->data != NULL)
        b->data[0] = '\0';
}

/* memmem 은 POSIX 가 아니므로 memchr(첫 바이트) + memcmp 로 직접 구현한다. */
ssize_t buf_find(const buf_t *b, size_t from, const void *needle, size_t nlen)
{
    const unsigned char *nd = needle;
    const char *hit;

    if (from > b->len)
        return -1;
    if (nlen == 0)
        return (ssize_t)from;
    while (nlen <= b->len - from) {
        hit = memchr(b->data + from, nd[0], b->len - from - nlen + 1);
        if (hit == NULL)
            return -1;
        from = (size_t)(hit - b->data);
        if (memcmp(hit, nd, nlen) == 0)
            return (ssize_t)from;
        from++;
    }
    return -1;
}
