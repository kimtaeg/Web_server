/*
 * test_timer_heap.c — 최소 힙: 기본 순서, set 으로 마감 변경, 중간 삭제, 무작위 연산 후 불변식.
 */
#include "test.h"

#include <stdlib.h>

#include "timer_heap.h"

typedef struct {
    size_t slot;
    long long when;
    int id;
} item_t;

/* peek 으로 하나씩 꺼내며 마감이 단조 증가하는지 확인하고, 꺼낸 순서의 id 를 out 에 쓴다 */
static int drain(timer_heap_t *h, int *out, int max)
{
    int k = 0;
    long long prev = -1;

    while (timer_heap_size(h) > 0 && k < max) {
        void *o;
        long long d = timer_heap_peek(h, &o);
        item_t *it = o;

        if (d < prev)
            return -1;
        prev = d;
        out[k++] = it->id;
        timer_heap_remove(h, &it->slot);
        CHECK_EQ_SIZE(it->slot, TIMER_NONE);
    }
    return k;
}

static void test_order(void)
{
    timer_heap_t h;
    item_t it[5] = {{TIMER_NONE, 50, 0}, {TIMER_NONE, 10, 1}, {TIMER_NONE, 40, 2},
                    {TIMER_NONE, 20, 3}, {TIMER_NONE, 30, 4}};
    int order[5];
    int i;

    timer_heap_init(&h);
    CHECK_EQ_INT((int)timer_heap_peek(&h, NULL), -1); /* 빈 힙 */
    for (i = 0; i < 5; i++)
        REQUIRE(timer_heap_set(&h, it[i].when, &it[i], &it[i].slot) == 0);
    CHECK(timer_heap_check(&h));
    CHECK_EQ_SIZE(timer_heap_size(&h), 5);
    CHECK_EQ_INT(drain(&h, order, 5), 5);
    CHECK_EQ_INT(order[0], 1);
    CHECK_EQ_INT(order[1], 3);
    CHECK_EQ_INT(order[2], 4);
    CHECK_EQ_INT(order[3], 2);
    CHECK_EQ_INT(order[4], 0);
    timer_heap_free(&h);
}

static void test_update_and_remove(void)
{
    timer_heap_t h;
    item_t a = {TIMER_NONE, 100, 0}, b = {TIMER_NONE, 200, 1}, c = {TIMER_NONE, 300, 2};
    void *o;

    timer_heap_init(&h);
    timer_heap_set(&h, a.when, &a, &a.slot);
    timer_heap_set(&h, b.when, &b, &b.slot);
    timer_heap_set(&h, c.when, &c, &c.slot);

    /* 이미 있는 노드에 set → 추가가 아니라 변경 (연결의 유휴 타이머 연장) */
    timer_heap_set(&h, 400, &a, &a.slot);
    CHECK_EQ_SIZE(timer_heap_size(&h), 3);
    CHECK(timer_heap_check(&h));
    CHECK_EQ_INT((int)timer_heap_peek(&h, &o), 200);
    CHECK(o == &b);

    /* 앞당기기 */
    timer_heap_set(&h, 5, &c, &c.slot);
    CHECK_EQ_INT((int)timer_heap_peek(&h, &o), 5);
    CHECK(o == &c);

    /* 중간(맨 앞 아닌) 노드 삭제, 두 번 삭제해도 안전 */
    timer_heap_remove(&h, &b.slot);
    timer_heap_remove(&h, &b.slot);
    CHECK_EQ_SIZE(timer_heap_size(&h), 2);
    CHECK(timer_heap_check(&h));

    /* free 하면 남은 owner 의 slot 이 TIMER_NONE 으로 */
    timer_heap_free(&h);
    CHECK_EQ_SIZE(a.slot, TIMER_NONE);
    CHECK_EQ_SIZE(c.slot, TIMER_NONE);
}

/* 무작위 set/remove 를 많이 섞고 매번 불변식을 확인한다. 이동 중 slot 갱신을 하나라도 빠뜨리면 실패. */
static void test_random(void)
{
    enum { N = 500, OPS = 20000 };
    static item_t it[N];
    timer_heap_t h;
    int order[N];
    int i, ok = 1;
    unsigned seed = 12345;

    timer_heap_init(&h);
    for (i = 0; i < N; i++) {
        it[i].slot = TIMER_NONE;
        it[i].id = i;
    }
    for (i = 0; i < OPS && ok; i++) {
        int k;

        seed = seed * 1103515245u + 12345u;
        k = (int)((seed >> 8) % N);
        if ((seed >> 4) % 4 == 0) {
            timer_heap_remove(&h, &it[k].slot);
        } else {
            it[k].when = (long long)((seed >> 12) % 100000);
            if (timer_heap_set(&h, it[k].when, &it[k], &it[k].slot) != 0)
                ok = 0;
        }
        if (!timer_heap_check(&h))
            ok = 0;
    }
    CHECK(ok);
    CHECK(drain(&h, order, N) >= 0); /* 꺼내는 순서가 단조 증가 */
    CHECK_EQ_SIZE(timer_heap_size(&h), 0);
    timer_heap_free(&h);
}

int main(void)
{
    RUN_TEST(test_order);
    RUN_TEST(test_update_and_remove);
    RUN_TEST(test_random);
    TEST_END();
}
