/*
 * test_rproxy.c — RR 순서, down 백엔드 건너뛰기/복귀, exclude, 동시 호출 시 분배, 풀 초기화.
 */
#include "test.h"

#include <pthread.h>

#include "rproxy.h"

static rproxy_pool_t pool;

static void make_pool(size_t n, long down_secs)
{
    config_hostport_t hps[CONFIG_MAX_BACKENDS];
    char err[128];
    size_t i;

    for (i = 0; i < n; i++) {
        snprintf(hps[i].host, sizeof hps[i].host, "127.0.0.1");
        hps[i].port = 9000 + (int)i;
    }
    if (rproxy_pool_init(&pool, hps, n, 100, 100, down_secs, err, sizeof err) != 0)
        fprintf(stderr, "    pool init: %s\n", err);
}

static void test_round_robin_order(void)
{
    char seq[16];
    int i;

    make_pool(3, 10);
    for (i = 0; i < 9; i++)
        seq[i] = (char)('A' + rproxy_pick(&pool, 1000, -1));
    seq[9] = '\0';
    CHECK_EQ_STR(seq, "ABCABCABC");
    CHECK_EQ_STR(pool.backends[1].name, "127.0.0.1:9001");
}

static void test_skip_down_and_recover(void)
{
    char seq[16];
    int i;

    make_pool(3, 10); /* down 10초 */
    rproxy_mark_down(&pool, 1, 1000); /* B 는 1000+10000 = 11000 까지 down */
    for (i = 0; i < 6; i++)
        seq[i] = (char)('A' + rproxy_pick(&pool, 5000, -1));
    seq[6] = '\0';
    /* 시작점 A,B,C,A,B,C 중 B 차례는 다음 정상 백엔드 C 로 */
    CHECK_EQ_STR(seq, "ACCACC");

    /* down 기간이 지나면 다시 순환에 들어온다 */
    for (i = 0; i < 3; i++)
        seq[i] = (char)('A' + rproxy_pick(&pool, 11000, -1));
    seq[3] = '\0';
    CHECK_EQ_STR(seq, "ABC");
}

static void test_all_down_and_exclude(void)
{
    make_pool(2, 10);
    rproxy_mark_down(&pool, 0, 0);
    rproxy_mark_down(&pool, 1, 0);
    CHECK_EQ_INT(rproxy_pick(&pool, 5000, -1), -1); /* 전부 down → -1 (호출자가 즉시 502) */
    CHECK(rproxy_pick(&pool, 10000, -1) >= 0);      /* 정확히 down 이 끝나는 시각부터 복귀 */

    make_pool(2, 10);
    CHECK_EQ_INT(rproxy_pick(&pool, 0, 0), 1); /* 재시도: 방금 실패한 0 은 제외 */
    CHECK_EQ_INT(rproxy_pick(&pool, 0, 0), 1);
    rproxy_mark_down(&pool, 1, 0);
    CHECK_EQ_INT(rproxy_pick(&pool, 0, 0), -1); /* 남은 게 없음 */

    make_pool(1, 0); /* down_secs 0: 표시해도 즉시 복귀 */
    rproxy_mark_down(&pool, 0, 500);
    CHECK_EQ_INT(rproxy_pick(&pool, 500, -1), 0);
}

/* 원자 카운터: 여러 스레드가 동시에 골라도 정확히 고르게 분배된다 (락 없이). */
#define THREADS 8
#define PICKS 30000
static int counts[THREADS][3];

static void *picker(void *arg)
{
    int t = (int)(long)arg;
    int i;

    for (i = 0; i < PICKS; i++)
        counts[t][rproxy_pick(&pool, 0, -1)]++;
    return NULL;
}

static void test_concurrent_distribution(void)
{
    pthread_t th[THREADS];
    long total[3] = {0, 0, 0};
    long i;
    int k;

    make_pool(3, 10);
    memset(counts, 0, sizeof counts);
    for (i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, picker, (void *)i);
    for (i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    for (i = 0; i < THREADS; i++)
        for (k = 0; k < 3; k++)
            total[k] += counts[i][k];
    /* 총 240000 회 = 3 으로 나누어떨어짐 → 각각 정확히 80000 (카운터가 원자적이면) */
    CHECK_EQ_INT(total[0], THREADS * PICKS / 3);
    CHECK_EQ_INT(total[1], THREADS * PICKS / 3);
    CHECK_EQ_INT(total[2], THREADS * PICKS / 3);
}

static void test_pool_init(void)
{
    config_hostport_t hps[2] = {{"localhost", 8080}, {"::1", 9090}};
    char err[256] = "";

    CHECK_EQ_INT(rproxy_pool_init(&pool, hps, 2, 1, 1, 1, err, sizeof err), 0);
    CHECK_EQ_STR(pool.backends[1].name, "[::1]:9090");

    snprintf(hps[0].host, sizeof hps[0].host, "no-such-host.invalid");
    CHECK_EQ_INT(rproxy_pool_init(&pool, hps, 1, 1, 1, 1, err, sizeof err), -1);
    CHECK(strstr(err, "no-such-host.invalid") != NULL);
    CHECK_EQ_INT(rproxy_pool_init(&pool, hps, 0, 1, 1, 1, err, sizeof err), -1);
}

int main(void)
{
    RUN_TEST(test_round_robin_order);
    RUN_TEST(test_skip_down_and_recover);
    RUN_TEST(test_all_down_and_exclude);
    RUN_TEST(test_concurrent_distribution);
    RUN_TEST(test_pool_init);
    TEST_END();
}
