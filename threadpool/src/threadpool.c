/*
 * threadpool.c — 고정 워커 + bounded queue. 설계와 동기화 방식은 threadpool.h 참고.
 */
#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

struct threadpool {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    /* --- mu 로 보호 --- */
    threadpool_item_t *q; /* 원형 버퍼 */
    size_t cap;
    size_t head; /* 다음에 꺼낼 위치 */
    size_t count;
    int shutting_down;
    /* --- 생성 후 불변 --- */
    pthread_t *threads;
    size_t nthreads;
    threadpool_fn fn;
    void *arg;
};

static void *worker_main(void *p)
{
    threadpool_t *tp = p;

    for (;;) {
        threadpool_item_t item;

        pthread_mutex_lock(&tp->mu);
        /* while: 가짜 깨어남(spurious wakeup)과, 다른 워커가 먼저 가져간 경우를 모두 막는다 */
        while (tp->count == 0 && !tp->shutting_down)
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        if (tp->count == 0) { /* 종료 중이고 남은 일도 없음 */
            pthread_mutex_unlock(&tp->mu);
            return NULL;
        }
        item = tp->q[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mu);

        tp->fn(item, tp->arg); /* 락 밖에서 실행: 작업이 오래 걸려도 다른 워커를 막지 않는다 */
    }
}

/* mu 를 잡은 상태에서 호출. */
static void push_locked(threadpool_t *tp, threadpool_item_t item)
{
    tp->q[(tp->head + tp->count) % tp->cap] = item;
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
}

threadpool_t *threadpool_create(size_t nworkers, size_t qcap, threadpool_fn fn, void *arg)
{
    threadpool_t *tp = NULL;
    size_t i;
    int mu_ok = 0, ne_ok = 0, nf_ok = 0;

    if (nworkers == 0 || qcap == 0 || fn == NULL)
        return NULL;
    tp = calloc(1, sizeof *tp);
    if (tp == NULL)
        goto fail;
    tp->q = calloc(qcap, sizeof *tp->q);
    tp->threads = calloc(nworkers, sizeof *tp->threads);
    if (tp->q == NULL || tp->threads == NULL)
        goto fail;
    tp->cap = qcap;
    tp->fn = fn;
    tp->arg = arg;
    if (pthread_mutex_init(&tp->mu, NULL) != 0)
        goto fail;
    mu_ok = 1;
    if (pthread_cond_init(&tp->not_empty, NULL) != 0)
        goto fail;
    ne_ok = 1;
    if (pthread_cond_init(&tp->not_full, NULL) != 0)
        goto fail;
    nf_ok = 1;

    for (i = 0; i < nworkers; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker_main, tp) != 0)
            goto fail;
        tp->nthreads++;
    }
    return tp;

fail:
    if (tp != NULL) {
        if (tp->nthreads > 0) { /* 일부 워커만 만들어졌으면 정상 종료 경로로 정리 */
            threadpool_destroy(tp);
            return NULL;
        }
        if (nf_ok)
            pthread_cond_destroy(&tp->not_full);
        if (ne_ok)
            pthread_cond_destroy(&tp->not_empty);
        if (mu_ok)
            pthread_mutex_destroy(&tp->mu);
        free(tp->threads);
        free(tp->q);
        free(tp);
    }
    return NULL;
}

int threadpool_try_submit(threadpool_t *tp, threadpool_item_t item)
{
    int rc = -1;

    pthread_mutex_lock(&tp->mu);
    if (!tp->shutting_down && tp->count < tp->cap) {
        push_locked(tp, item);
        rc = 0;
    }
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

int threadpool_submit_wait(threadpool_t *tp, threadpool_item_t item)
{
    int rc = -1;

    pthread_mutex_lock(&tp->mu);
    while (tp->count == tp->cap && !tp->shutting_down)
        pthread_cond_wait(&tp->not_full, &tp->mu);
    if (!tp->shutting_down) {
        push_locked(tp, item);
        rc = 0;
    }
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

void threadpool_close(threadpool_t *tp)
{
    if (tp == NULL)
        return;
    pthread_mutex_lock(&tp->mu);
    tp->shutting_down = 1;
    pthread_cond_broadcast(&tp->not_empty); /* 자고 있는 워커 전부: 남은 일 처리 후 종료 */
    pthread_cond_broadcast(&tp->not_full);  /* submit_wait 대기자: -1 로 빠져나감 */
    pthread_mutex_unlock(&tp->mu);
}

void threadpool_destroy(threadpool_t *tp)
{
    threadpool_close(tp);
    threadpool_join_free(tp);
}

void threadpool_join_free(threadpool_t *tp)
{
    size_t i;

    if (tp == NULL)
        return;
    threadpool_close(tp); /* 멱등. close 를 빠뜨리고 불러도 워커가 끝나지 않아 영원히 join 하는 일이 없게 */
    for (i = 0; i < tp->nthreads; i++)
        pthread_join(tp->threads[i], NULL);

    pthread_cond_destroy(&tp->not_full);
    pthread_cond_destroy(&tp->not_empty);
    pthread_mutex_destroy(&tp->mu);
    free(tp->threads);
    free(tp->q);
    free(tp);
}
