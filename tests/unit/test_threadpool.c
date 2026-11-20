/*
 * test_threadpool.c — 작업 실행, 큐 포화 시 try_submit 실패, submit_wait 대기, graceful 종료.
 */
#include "test.h"

#include <pthread.h>
#include <time.h>

#include "threadpool.h"

/* 작업 함수가 "게이트"에서 막히게 해서 큐 상태를 결정적으로 만든다. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int gate_open;
static int started;
static int done;
static long sum;

static void reset(int open)
{
    pthread_mutex_lock(&mu);
    gate_open = open;
    started = done = 0;
    sum = 0;
    pthread_mutex_unlock(&mu);
}

static void job(int item, void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    started++;
    pthread_cond_broadcast(&cv);
    while (!gate_open)
        pthread_cond_wait(&cv, &mu);
    sum += item;
    done++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void wait_started(int n)
{
    pthread_mutex_lock(&mu);
    while (started < n)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void open_gate(void)
{
    pthread_mutex_lock(&mu);
    gate_open = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void test_all_jobs_run(void)
{
    threadpool_t *tp;
    int i;
    long want = 0;

    reset(1);
    tp = threadpool_create(4, 16, job, NULL);
    REQUIRE(tp != NULL);
    for (i = 1; i <= 1000; i++) {
        CHECK_EQ_INT(threadpool_submit_wait(tp, i), 0);
        want += i;
    }
    threadpool_destroy(tp); /* 큐에 남은 것까지 처리 후 종료 */
    CHECK_EQ_INT(done, 1000);
    CHECK(sum == want);
}

static void test_invalid_args(void)
{
    CHECK(threadpool_create(0, 4, job, NULL) == NULL);
    CHECK(threadpool_create(1, 0, job, NULL) == NULL);
    CHECK(threadpool_create(1, 1, NULL, NULL) == NULL);
}

typedef struct {
    threadpool_t *tp;
    int rc;
    int returned;
} waiter_t;

static void *waiter_main(void *p)
{
    waiter_t *w = p;

    w->rc = threadpool_submit_wait(w->tp, 100);
    pthread_mutex_lock(&mu);
    w->returned = 1;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/*
 * 워커 1, 큐 2:
 *   작업 1 → 워커가 잡고 게이트에서 대기
 *   작업 2, 3 → 큐에 쌓임 (가득 참)
 *   작업 4 → try_submit 즉시 실패 (main 스레드라면 503)
 *   submit_wait(100) → not_full 에서 대기하다가 게이트가 열리면 들어감
 */
static void test_full_queue(void)
{
    threadpool_t *tp;
    pthread_t th;
    waiter_t w;
    struct timespec ts = {0, 100 * 1000 * 1000};
    int returned_early;

    reset(0);
    tp = threadpool_create(1, 2, job, NULL);
    REQUIRE(tp != NULL);
    CHECK_EQ_INT(threadpool_try_submit(tp, 1), 0);
    wait_started(1);
    CHECK_EQ_INT(threadpool_try_submit(tp, 2), 0);
    CHECK_EQ_INT(threadpool_try_submit(tp, 3), 0);
    CHECK_EQ_INT(threadpool_try_submit(tp, 4), -1);

    w.tp = tp;
    w.rc = -99;
    w.returned = 0;
    pthread_create(&th, NULL, waiter_main, &w);
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&mu);
    returned_early = w.returned;
    pthread_mutex_unlock(&mu);
    CHECK_EQ_INT(returned_early, 0); /* 큐가 찼으니 아직 대기 중이어야 함 */

    open_gate();
    pthread_join(th, NULL);
    CHECK_EQ_INT(w.rc, 0);
    threadpool_destroy(tp);
    CHECK_EQ_INT(done, 4);
    CHECK(sum == 1 + 2 + 3 + 100);
}

/* 종료가 시작되면 submit_wait 대기자는 -1 로 풀려나야 한다 (영원히 막히면 안 됨). */
static void test_destroy_releases_waiter(void)
{
    threadpool_t *tp;
    pthread_t th;
    waiter_t w;
    struct timespec ts = {0, 50 * 1000 * 1000};

    reset(0);
    tp = threadpool_create(1, 1, job, NULL);
    REQUIRE(tp != NULL);
    threadpool_try_submit(tp, 1);
    wait_started(1);
    threadpool_try_submit(tp, 2); /* 큐 가득 */
    w.tp = tp;
    w.rc = -99;
    w.returned = 0;
    pthread_create(&th, NULL, waiter_main, &w);
    nanosleep(&ts, NULL);
    open_gate();
    threadpool_destroy(tp);
    pthread_join(th, NULL);
    /* 대기자는 자리가 나서 들어갔거나(0), 종료로 거절됐거나(-1) 둘 중 하나. 막히지 않은 것이 핵심 */
    CHECK(w.rc == 0 || w.rc == -1);
    CHECK_EQ_INT(done, w.rc == 0 ? 3 : 2);
}

int main(void)
{
    RUN_TEST(test_all_jobs_run);
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_full_queue);
    RUN_TEST(test_destroy_releases_waiter);
    TEST_END();
}
