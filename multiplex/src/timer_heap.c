/*
 * timer_heap.c — 배열 기반 이진 최소 힙. 설계는 timer_heap.h 참고.
 * 부모 = (i-1)/2, 자식 = 2i+1, 2i+2.
 */
#include "timer_heap.h"

#include <stdlib.h>

void timer_heap_init(timer_heap_t *h)
{
    h->a = NULL;
    h->n = 0;
    h->cap = 0;
}

void timer_heap_free(timer_heap_t *h)
{
    size_t i;

    for (i = 0; i < h->n; i++) /* 남은 owner 들이 해제된 힙을 가리키지 않도록 */
        *h->a[i].slot = TIMER_NONE;
    free(h->a);
    timer_heap_init(h);
}

/* 노드를 i 에 놓고 owner 의 slot 을 갱신한다. 모든 이동은 이 함수를 거친다. */
static void place(timer_heap_t *h, size_t i, timer_node_t node)
{
    h->a[i] = node;
    *node.slot = i;
}

static void sift_up(timer_heap_t *h, size_t i)
{
    timer_node_t node = h->a[i];

    while (i > 0) {
        size_t parent = (i - 1) / 2;

        if (h->a[parent].deadline_ms <= node.deadline_ms)
            break;
        place(h, i, h->a[parent]);
        i = parent;
    }
    place(h, i, node);
}

static void sift_down(timer_heap_t *h, size_t i)
{
    timer_node_t node = h->a[i];

    for (;;) {
        size_t l = 2 * i + 1;
        size_t r = l + 1;
        size_t m;

        if (l >= h->n)
            break;
        m = (r < h->n && h->a[r].deadline_ms < h->a[l].deadline_ms) ? r : l;
        if (node.deadline_ms <= h->a[m].deadline_ms)
            break;
        place(h, i, h->a[m]);
        i = m;
    }
    place(h, i, node);
}

int timer_heap_set(timer_heap_t *h, long long deadline_ms, void *owner, size_t *slot)
{
    size_t i = *slot;

    if (i != TIMER_NONE) {
        long long old = h->a[i].deadline_ms;

        h->a[i].deadline_ms = deadline_ms;
        if (deadline_ms < old)
            sift_up(h, i);
        else if (deadline_ms > old)
            sift_down(h, i);
        return 0;
    }
    if (h->n == h->cap) {
        size_t ncap = h->cap ? h->cap * 2 : 64;
        timer_node_t *na;

        if (ncap > SIZE_MAX / sizeof *na)
            return -1;
        na = realloc(h->a, ncap * sizeof *na);
        if (na == NULL)
            return -1;
        h->a = na;
        h->cap = ncap;
    }
    h->a[h->n].deadline_ms = deadline_ms;
    h->a[h->n].owner = owner;
    h->a[h->n].slot = slot;
    *slot = h->n;
    h->n++;
    sift_up(h, h->n - 1);
    return 0;
}

void timer_heap_remove(timer_heap_t *h, size_t *slot)
{
    size_t i = *slot;
    timer_node_t last;

    if (i == TIMER_NONE)
        return;
    *slot = TIMER_NONE;
    h->n--;
    if (i == h->n) /* 마지막 노드였으면 끝 */
        return;
    /* 마지막 노드를 빈자리로 옮긴 뒤, 위나 아래 중 맞는 방향으로 한 번만 움직인다 */
    last = h->a[h->n];
    place(h, i, last);
    if (i > 0 && h->a[(i - 1) / 2].deadline_ms > last.deadline_ms)
        sift_up(h, i);
    else
        sift_down(h, i);
}

long long timer_heap_peek(const timer_heap_t *h, void **owner)
{
    if (h->n == 0)
        return -1;
    if (owner != NULL)
        *owner = h->a[0].owner;
    return h->a[0].deadline_ms;
}

size_t timer_heap_size(const timer_heap_t *h)
{
    return h->n;
}

int timer_heap_check(const timer_heap_t *h)
{
    size_t i;

    for (i = 0; i < h->n; i++) {
        if (*h->a[i].slot != i)
            return 0;
        if (i > 0 && h->a[(i - 1) / 2].deadline_ms > h->a[i].deadline_ms)
            return 0;
    }
    return 1;
}
