#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct {
    job_fn fn;
    void *arg;
} job_t;

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    job_t *queue;
    size_t cap, head, count;
    pthread_t *threads;
    int nthreads;
    int shutdown;
};

static void *worker_main(void *arg) {
    threadpool_t *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (p->count == 0 && !p->shutdown)
            pthread_cond_wait(&p->not_empty, &p->lock);
        if (p->count == 0 && p->shutdown) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        job_t job = p->queue[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
        pthread_mutex_unlock(&p->lock);
        job.fn(job.arg);
    }
}

threadpool_t *threadpool_create(int nthreads, size_t queue_cap) {
    threadpool_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->queue = calloc(queue_cap, sizeof(job_t));
    p->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!p->queue || !p->threads)
        goto fail;
    p->cap = queue_cap;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker_main, p) != 0) {
            p->nthreads = i;
            threadpool_destroy(p);
            return NULL;
        }
    }
    p->nthreads = nthreads;
    return p;
fail:
    free(p->queue);
    free(p->threads);
    free(p);
    return NULL;
}

int threadpool_submit(threadpool_t *p, job_fn fn, void *arg) {
    pthread_mutex_lock(&p->lock);
    if (p->shutdown || p->count == p->cap) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    p->queue[(p->head + p->count) % p->cap] = (job_t){fn, arg};
    p->count++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void threadpool_destroy(threadpool_t *p) {
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->not_empty);
    free(p->queue);
    free(p->threads);
    free(p);
}

size_t threadpool_pending(threadpool_t *p) {
    pthread_mutex_lock(&p->lock);
    size_t n = p->count;
    pthread_mutex_unlock(&p->lock);
    return n;
}

int threadpool_size(threadpool_t *p) {
    return p->nthreads;
}
