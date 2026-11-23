#ifndef SERVER_H
#define SERVER_H

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>

#include "buffer.h"
#include "cache.h"
#include "config.h"
#include "http.h"
#include "threadpool.h"

typedef struct server server_t;

typedef struct conn {
    int fd;
    char peer[64];
    buf_t in;              /* bytes received but not yet parsed */
    long long last_active; /* now_ms() when it last finished a request */
    int requests;
    server_t *srv;
    struct conn *next;     /* link in the reactor's return list */
} conn_t;

typedef struct {
    _Atomic unsigned long long requests;
    _Atomic unsigned long long connections;
    _Atomic long long active_connections;
    _Atomic unsigned long long bytes_out;
    _Atomic unsigned long long by_class[6]; /* index = status / 100 */
    _Atomic unsigned long long rejected;    /* 503 at accept: over capacity */
} server_stats_t;

struct server {
    config_t *cfg;
    int listen_fd;
    int wake_pipe[2]; /* workers hand connections back; signals stop the loop */
    threadpool_t *pool;
    cache_t *cache;

    struct pollfd *pfds; /* [0] listen, [1] wake pipe, [2..] idle keep-alive conns */
    conn_t **pconns;
    size_t npoll, cap;

    pthread_mutex_t ret_lock;
    conn_t *returned;

    long long started_at;
    server_stats_t stats;
};

/* Per-request state shared by every handler; filled in for the access log. */
typedef struct {
    server_t *srv;
    conn_t *conn;
    http_request_t *req;
    int status;
    long long bytes;
    const char *handler;
    char upstream[300];
    int keep_alive;
} req_ctx_t;

int server_init(server_t *s, config_t *cfg, char *err, size_t err_cap);
int server_run(server_t *s);
void server_destroy(server_t *s);

void server_install_signals(server_t *s);
int server_stopping(void);

#endif
