#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>

typedef void (*job_fn)(void *arg);
typedef struct threadpool threadpool_t;

/* Fixed worker threads fed from a bounded FIFO queue. */
threadpool_t *threadpool_create(int nthreads, size_t queue_cap);
/* Returns -1 when the queue is full or the pool is shutting down. */
int threadpool_submit(threadpool_t *p, job_fn fn, void *arg);
/* Runs every queued job, then joins the workers. */
void threadpool_destroy(threadpool_t *p);
size_t threadpool_pending(threadpool_t *p);
int threadpool_size(threadpool_t *p);

#endif
