/*
 * test_cache.c — LRU 축출 순서, 크기 제한, 교체, 만료, 동시 접근(get/put 혼합 + 무결성).
 */
#include "test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "cache.h"

/* 항목 비용 = 고정 오버헤드 + 키 + 값. 근사값이 아니라 실제 값으로 경계를 정확히 계산한다. */
#define OV (cache_entry_overhead())

static int has(cache_t *c, const char *k)
{
    buf_t b;
    int rc;

    buf_init(&b, 0, 1 << 20);
    rc = cache_get(c, k, strlen(k), 0, &b, NULL);
    buf_free(&b);
    return rc == CACHE_HIT;
}

static void put(cache_t *c, const char *k, size_t vlen)
{
    char *v = malloc(vlen + 1);

    memset(v, 'v', vlen);
    CHECK_EQ_INT(cache_put(c, k, strlen(k), v, vlen, 0, 1LL << 60), 0);
    free(v);
}

static void test_basic_get_put(void)
{
    cache_t *c = cache_create(1 << 20, 1 << 16, 16);
    buf_t b;
    size_t aux = 0;

    REQUIRE(c != NULL);
    buf_init(&b, 0, 1 << 16);
    CHECK_EQ_INT(cache_get(c, "k", 1, 0, &b, &aux), CACHE_MISS);
    CHECK_EQ_INT(cache_put(c, "k", 1, "hello", 5, 42, 1000), 0);
    CHECK_EQ_INT(cache_get(c, "k", 1, 0, &b, &aux), CACHE_HIT);
    CHECK_EQ_SIZE(b.len, 5);
    CHECK_EQ_MEM(b.data, "hello", 5);
    CHECK_EQ_SIZE(aux, 42);
    CHECK_EQ_INT(cache_get(c, "K", 1, 0, &b, &aux), CACHE_MISS); /* 키는 대소문자 구분 */
    CHECK_EQ_INT(cache_get(c, "kk", 2, 0, &b, &aux), CACHE_MISS);
    CHECK_EQ_INT(cache_self_check(c), 0);
    buf_free(&b);
    cache_destroy(c);
}

/* 고전적 LRU 시나리오: A B C 를 넣고 A 를 읽은 뒤 D 를 넣으면 가장 오래 안 쓴 B 가 빠진다. */
static void test_lru_eviction_order(void)
{
    /* 정확히 항목 3개분 용량 (키 1바이트 + 값 100바이트) */
    size_t one = OV + 1 + 100;
    cache_t *c = cache_create(3 * one, 1000, 1); /* 버킷 1개: 체이닝 경로도 함께 검사 */
    cache_stats_t st;

    REQUIRE(c != NULL);
    put(c, "A", 100);
    put(c, "B", 100);
    put(c, "C", 100); /* LRU 순서: C B A (A 가 가장 오래됨) */
    CHECK(has(c, "A")); /* A 사용 → A C B (B 가 가장 오래됨) */
    put(c, "D", 100);   /* 넘침 → B 축출 */
    CHECK(!has(c, "B"));
    CHECK(has(c, "A") && has(c, "C") && has(c, "D"));
    cache_stats(c, &st);
    CHECK_EQ_INT(st.evictions, 1);
    CHECK_EQ_SIZE(st.entries, 3);

    /* 지금 순서: D C A (위의 has 호출 순서: A, C, D) → E 를 넣으면 A 축출 */
    put(c, "E", 100);
    CHECK(!has(c, "A"));
    CHECK(has(c, "C") && has(c, "D") && has(c, "E"));
    CHECK_EQ_INT(cache_self_check(c), 0);
    cache_destroy(c);
}

static void test_size_limits(void)
{
    size_t one = OV + 1 + 100;
    cache_t *c = cache_create(5 * one, 300, 64);
    char big[1000];
    cache_stats_t st;

    REQUIRE(c != NULL);
    memset(big, 'b', sizeof big);
    /* 객체 상한(300) 초과는 저장 거부 */
    CHECK_EQ_INT(cache_put(c, "big", 3, big, 301, 0, 1LL << 60), CACHE_TOO_BIG);
    CHECK_EQ_INT(cache_put(c, "ok", 2, big, 300, 0, 1LL << 60), 0);
    CHECK(!has(c, "big"));
    CHECK(has(c, "ok"));

    /* 전체 상한: 작은 것 5개로 가득 찬 상태에서, 비용이 정확히 3개분인 큰 항목 L 이 들어오면
     * 가장 오래된 것부터 정확히 3개가 연쇄 축출된다 (2개만 빼면 넘치고, 3개 빼면 딱 맞음) */
    cache_destroy(c);
    c = cache_create(5 * one, sizeof big, 64);
    put(c, "1", 100);
    put(c, "2", 100);
    put(c, "3", 100);
    put(c, "4", 100);
    put(c, "5", 100); /* 가득 참 */
    REQUIRE(3 * one - OV - 1 <= sizeof big);
    CHECK_EQ_INT(cache_put(c, "L", 1, big, 3 * one - OV - 1, 0, 1LL << 60), 0);
    CHECK(!has(c, "1") && !has(c, "2") && !has(c, "3"));
    CHECK(has(c, "4") && has(c, "5") && has(c, "L"));
    cache_stats(c, &st);
    CHECK_EQ_INT(st.evictions, 3);
    CHECK_EQ_SIZE(st.bytes, 5 * one); /* 정확히 가득 찬 상태 */
    CHECK_EQ_INT(cache_self_check(c), 0);

    /* 전체 상한보다 큰 객체는 (객체 상한 안이라도) 거부 */
    cache_destroy(c);
    c = cache_create(200, 100000, 4);
    CHECK_EQ_INT(cache_put(c, "x", 1, big, 500, 0, 1LL << 60), CACHE_TOO_BIG);
    cache_destroy(c);
}

static void test_replace_and_expire(void)
{
    cache_t *c = cache_create(1 << 20, 1 << 16, 8);
    buf_t b;
    cache_stats_t st;

    REQUIRE(c != NULL);
    buf_init(&b, 0, 1 << 16);
    cache_put(c, "k", 1, "old-value", 9, 1, 1LL << 60);
    cache_put(c, "k", 1, "new", 3, 2, 1LL << 60); /* 같은 키 교체 */
    cache_stats(c, &st);
    CHECK_EQ_SIZE(st.entries, 1);
    CHECK_EQ_INT(cache_get(c, "k", 1, 0, &b, NULL), CACHE_HIT);
    CHECK_EQ_SIZE(b.len, 3);
    CHECK_EQ_MEM(b.data, "new", 3);
    CHECK_EQ_INT(cache_self_check(c), 0); /* 교체 후 크기 합계가 맞는가 */

    /* 만료: expires_ms 이상이면 MISS, 항목도 제거 */
    cache_put(c, "t", 1, "ttl", 3, 0, 1000);
    buf_reset(&b);
    CHECK_EQ_INT(cache_get(c, "t", 1, 999, &b, NULL), CACHE_HIT);
    CHECK_EQ_INT(cache_get(c, "t", 1, 1000, &b, NULL), CACHE_MISS);
    cache_stats(c, &st);
    CHECK_EQ_SIZE(st.entries, 1); /* 만료 항목은 지워짐 */
    CHECK_EQ_INT(cache_self_check(c), 0);
    buf_free(&b);
    cache_destroy(c);
}

/*
 * 동시 접근: 8 스레드가 64개 키에 get/put/교체를 섞어서 수만 번.
 * 값은 "키에서 결정되는 패턴 + 버전"이라, 읽은 값이 찢어졌거나 다른 키의 값이면 바로 드러난다.
 * 끝난 뒤 리스트·해시·바이트 합계 일관성(cache_self_check). make asan 에서는 메모리 오류도 검사.
 */
#define T_THREADS 8
#define T_OPS 20000
#define T_KEYS 64
static cache_t *shared;
static _Atomic int corrupt;

static void make_val(int key, unsigned ver, char *v, size_t *n)
{
    size_t len = 32 + ((unsigned)key * 37u + ver) % 200u; /* 부호 없는 연산: 음수 나머지 방지 */
    size_t i;

    for (i = 0; i < len; i++)
        v[i] = (char)('a' + (key + (int)i) % 26);
    memcpy(v, &ver, sizeof ver); /* 앞 4바이트는 버전 — 패턴 검사에서 제외 */
    *n = len;
}

static void *worker(void *arg)
{
    unsigned seed = (unsigned)(long)arg * 2654435761u;
    char v[300];
    buf_t b;
    int i;

    buf_init(&b, 0, 4096);
    for (i = 0; i < T_OPS; i++) {
        int key;
        char k[16];
        int kl;

        seed = seed * 1103515245u + 12345u;
        key = (int)((seed >> 16) % T_KEYS);
        kl = snprintf(k, sizeof k, "key-%d", key);
        if ((seed >> 8) % 3 == 0) {
            size_t n;

            make_val(key, seed, v, &n);
            cache_put(shared, k, (size_t)kl, v, n, (size_t)key, 1LL << 60);
        } else {
            size_t aux = 0;

            buf_reset(&b);
            if (cache_get(shared, k, (size_t)kl, 0, &b, &aux) == CACHE_HIT) {
                size_t j;
                int bad = aux != (size_t)key || b.len < 32;

                for (j = sizeof(unsigned); !bad && j < b.len; j++)
                    if (b.data[j] != (char)('a' + (key + (int)j) % 26))
                        bad = 1;
                if (bad)
                    atomic_fetch_add(&corrupt, 1);
            }
        }
    }
    buf_free(&b);
    return NULL;
}

static void test_concurrent(void)
{
    pthread_t th[T_THREADS];
    cache_stats_t st;
    long i;

    /* 용량을 작게 잡아 축출이 계속 일어나게 한다 (가장 까다로운 경로) */
    shared = cache_create(8 * 1024, 1024, 16); /* 키 64개 × 평균 ~230B ≈ 15KB > 8KB → 축출 계속 발생 */
    REQUIRE(shared != NULL);
    atomic_store(&corrupt, 0);
    for (i = 0; i < T_THREADS; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (i = 0; i < T_THREADS; i++)
        pthread_join(th[i], NULL);
    cache_stats(shared, &st);
    CHECK_EQ_INT(atomic_load(&corrupt), 0);
    CHECK_EQ_INT(cache_self_check(shared), 0);
    CHECK(st.hits > 0 && st.misses > 0 && st.evictions > 0); /* 세 경로가 모두 실제로 실행됨 */
    CHECK_EQ_INT(st.hits + st.misses + st.stores, (unsigned long long)T_THREADS * T_OPS);
    printf("    (hits=%llu misses=%llu stores=%llu evictions=%llu)\n", st.hits, st.misses,
           st.stores, st.evictions);
    cache_destroy(shared);
}

int main(void)
{
    RUN_TEST(test_basic_get_put);
    RUN_TEST(test_lru_eviction_order);
    RUN_TEST(test_size_limits);
    RUN_TEST(test_replace_and_expire);
    RUN_TEST(test_concurrent);
    TEST_END();
}
