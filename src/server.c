#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "handler.h"
#include "log.h"
#include "netutil.h"
#include "response.h"

#define LISTEN_BACKLOG 1024
#define READ_CHUNK 16384
#define STOP_CHECK_MS 250
#define LINGER_MS 500
#define LINGER_MAX_BYTES (4u * 1024 * 1024)

static volatile sig_atomic_t g_stop = 0;
static int g_wake_fd = -1;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    int saved = errno; /* write() is async-signal-safe; wake the reactor's poll() */
    if (g_wake_fd >= 0 && write(g_wake_fd, "s", 1) < 0) {
    }
    errno = saved;
}

int server_stopping(void) {
    return g_stop;
}

void server_install_signals(server_t *s) {
    g_wake_fd = s->wake_pipe[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* A client that disconnects mid-write must not kill the process. */
    signal(SIGPIPE, SIG_IGN);
}

static conn_t *conn_new(server_t *s, int fd, const struct sockaddr_storage *ss, socklen_t sl) {
    conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->srv = s;
    c->last_active = now_ms();
    buf_init(&c->in);
    if (getnameinfo((const struct sockaddr *)ss, sl, c->peer, sizeof(c->peer), NULL, 0,
                    NI_NUMERICHOST) != 0)
        snprintf(c->peer, sizeof(c->peer), "?");
    atomic_fetch_add(&s->stats.active_connections, 1);
    atomic_fetch_add(&s->stats.connections, 1);
    return c;
}

static void conn_close(conn_t *c) {
    atomic_fetch_sub(&c->srv->stats.active_connections, 1);
    close(c->fd);
    buf_free(&c->in);
    free(c);
}

/* After an error response the client may still be sending (e.g. a rejected upload).
 * Closing with unread data makes TCP send RST, which can destroy the response before the
 * client reads it, so half-close and drain briefly first. */
static void conn_close_lingering(conn_t *c) {
    shutdown(c->fd, SHUT_WR);
    char sink[READ_CHUNK];
    size_t drained = 0;
    long long deadline = now_ms() + LINGER_MS;
    while (drained < LINGER_MAX_BYTES) {
        int left = (int)(deadline - now_ms());
        if (left <= 0 || wait_fd(c->fd, POLLIN, left) <= 0)
            break;
        ssize_t n = recv_some(c->fd, sink, sizeof(sink));
        if (n <= 0)
            break;
        drained += (size_t)n;
    }
    conn_close(c);
}

/* ---- reactor-side poll set (only touched by the reactor thread) ---- */

static int poll_add(server_t *s, int fd, conn_t *c) {
    if (s->npoll == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 64;
        struct pollfd *p = realloc(s->pfds, ncap * sizeof(*p));
        if (!p)
            return -1;
        s->pfds = p;
        conn_t **cs = realloc(s->pconns, ncap * sizeof(*cs));
        if (!cs)
            return -1;
        s->pconns = cs;
        s->cap = ncap;
    }
    s->pfds[s->npoll] = (struct pollfd){.fd = fd, .events = POLLIN, .revents = 0};
    s->pconns[s->npoll] = c;
    s->npoll++;
    return 0;
}

static void poll_remove_at(server_t *s, size_t i) {
    size_t last = s->npoll - 1;
    s->pfds[i] = s->pfds[last];
    s->pconns[i] = s->pconns[last];
    s->npoll--;
}

static void return_to_reactor(conn_t *c) {
    server_t *s = c->srv;
    c->last_active = now_ms();
    pthread_mutex_lock(&s->ret_lock);
    c->next = s->returned;
    s->returned = c;
    pthread_mutex_unlock(&s->ret_lock);
    if (write(s->wake_pipe[1], "c", 1) < 0) {
        /* pipe full: the reactor already has a wake-up pending */
    }
}

/* ---- worker side ---- */

enum { READ_OK = 0, READ_EOF = -1, READ_TIMEOUT = -2, READ_STOP = -3 };

static int read_more(conn_t *c, long long deadline) {
    for (;;) {
        if (g_stop)
            return READ_STOP;
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return READ_TIMEOUT;
        int w = wait_fd(c->fd, POLLIN, left < STOP_CHECK_MS ? left : STOP_CHECK_MS);
        if (w < 0)
            return READ_EOF;
        if (w == 0)
            continue;
        if (buf_reserve(&c->in, READ_CHUNK) < 0)
            return READ_EOF;
        ssize_t n = recv_some(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len - 1);
        if (n <= 0)
            return READ_EOF; /* orderly close or reset: nothing left to answer */
        c->in.len += (size_t)n;
        c->in.data[c->in.len] = '\0';
        return READ_OK;
    }
}

static void record(req_ctx_t *ctx, long long started_us) {
    server_stats_t *st = &ctx->srv->stats;
    atomic_fetch_add(&st->requests, 1);
    atomic_fetch_add(&st->bytes_out, (unsigned long long)ctx->bytes);
    int cls = ctx->status / 100;
    if (cls >= 1 && cls <= 5)
        atomic_fetch_add(&st->by_class[cls], 1);
    const http_request_t *r = ctx->req;
    log_access(ctx->conn->peer, r ? r->method_str : NULL, r ? r->target : NULL,
               r ? r->version_minor : 1, ctx->status, ctx->bytes,
               now_us() - started_us, ctx->handler, ctx->upstream);
}

static void reply_without_request(conn_t *c, int status) {
    req_ctx_t ctx = {.srv = c->srv, .conn = c, .handler = "error", .keep_alive = 0};
    long long t0 = now_us();
    respond_error(&ctx, status, NULL);
    record(&ctx, t0);
}

/* One job = serve requests on this connection until it is idle, closed, or broken.
 * Pipelined requests already in the buffer are answered in order. */
static void conn_job(void *arg) {
    conn_t *c = arg;
    server_t *s = c->srv;
    const config_t *cfg = s->cfg;
    int served = 0, sent_continue = 0;
    long long deadline = now_ms() + (long long)cfg->request_timeout * 1000;

    for (;;) {
        http_request_t req;
        size_t consumed = 0;
        int status = 0;
        http_parse_result_t pr = http_parse_request(c->in.data ? c->in.data : "", c->in.len,
                                                    cfg->max_body, &req, &consumed, &status);
        if (pr == HTTP_PARSE_INCOMPLETE) {
            if (c->in.len == 0 && served > 0) {
                return_to_reactor(c);
                return;
            }
            if (status == 100 && !sent_continue) {
                static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (send_all(c->fd, cont, sizeof(cont) - 1) < 0)
                    break;
                sent_continue = 1;
            }
            int rr = read_more(c, deadline);
            if (rr == READ_OK)
                continue;
            if (rr == READ_TIMEOUT && c->in.len > 0) {
                reply_without_request(c, 408);
                conn_close_lingering(c);
                return;
            }
            break;
        }
        if (pr == HTTP_PARSE_ERROR) {
            reply_without_request(c, status);
            conn_close_lingering(c);
            return;
        }

        buf_consume(&c->in, consumed);
        req_ctx_t ctx = {.srv = s, .conn = c, .req = &req, .handler = "-"};
        ctx.keep_alive = req.keep_alive && !g_stop && cfg->keepalive_timeout > 0 &&
                         c->requests + 1 < cfg->max_keepalive_requests;
        long long t0 = now_us();
        handle_request(&ctx);
        record(&ctx, t0);
        http_request_free(&req);
        c->requests++;
        served++;
        sent_continue = 0;
        deadline = now_ms() + (long long)cfg->request_timeout * 1000;
        if (!ctx.keep_alive) {
            if (ctx.status >= 400)
                conn_close_lingering(c);
            else
                conn_close(c);
            return;
        }
    }
    conn_close(c);
}

/* ---- reactor ---- */

static void reject_busy(server_t *s, int fd) {
    static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                               "Retry-After: 1\r\nConnection: close\r\n\r\n";
    send_all(fd, busy, sizeof(busy) - 1);
    close(fd);
    atomic_fetch_add(&s->stats.rejected, 1);
}

static void accept_ready(server_t *s, long long *paused_until) {
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int fd = accept(s->listen_fd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* Out of descriptors: the listen socket stays readable, so stop polling it
                 * briefly instead of spinning. */
                log_error("accept: %s (pausing accepts)", strerror(errno));
                *paused_until = now_ms() + 100;
            }
            return; /* EAGAIN: backlog drained */
        }
        /* BSD/macOS sockets inherit O_NONBLOCK from the listener; Linux ones do not. */
        set_nonblocking(fd, 0);
        socket_tune(fd);
        set_io_timeouts(fd, s->cfg->request_timeout * 1000);
        if (atomic_load(&s->stats.active_connections) >= s->cfg->max_connections) {
            reject_busy(s, fd);
            continue;
        }
        conn_t *c = conn_new(s, fd, &ss, sl);
        if (!c) {
            close(fd);
            continue;
        }
        if (poll_add(s, fd, c) < 0)
            conn_close(c);
    }
}

static void take_returned(server_t *s) {
    char drain[256];
    while (read(s->wake_pipe[0], drain, sizeof(drain)) > 0) {
    }
    pthread_mutex_lock(&s->ret_lock);
    conn_t *list = s->returned;
    s->returned = NULL;
    pthread_mutex_unlock(&s->ret_lock);
    while (list) {
        conn_t *c = list;
        list = c->next;
        if (g_stop || poll_add(s, c->fd, c) < 0)
            conn_close(c);
    }
}

static void dispatch(server_t *s, conn_t *c) {
    if (threadpool_submit(s->pool, conn_job, c) < 0) {
        atomic_fetch_add(&s->stats.rejected, 1);
        reply_without_request(c, 503);
        conn_close(c);
    }
}

int server_run(server_t *s) {
    long long accept_paused_until = 0;
    log_info("listening on http://%s:%d  root=%s  threads=%d", s->cfg->bind_addr,
             s->cfg->port, s->cfg->root_real, s->cfg->threads);
    while (!g_stop) {
        long long now = now_ms();
        s->pfds[0].events = now >= accept_paused_until ? POLLIN : 0;
        int n = poll(s->pfds, (nfds_t)s->npoll, 1000);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll: %s", strerror(errno));
            break;
        }
        now = now_ms();
        for (size_t i = s->npoll; i-- > 2;) {
            conn_t *c = s->pconns[i];
            short re = s->pfds[i].revents;
            long long idle_limit = (long long)(c->requests ? s->cfg->keepalive_timeout
                                                           : s->cfg->request_timeout) * 1000;
            if (re) {
                poll_remove_at(s, i);
                if (re & (POLLERR | POLLNVAL))
                    conn_close(c);
                else
                    dispatch(s, c); /* POLLHUP may still carry a final request */
            } else if (now - c->last_active > idle_limit) {
                poll_remove_at(s, i);
                conn_close(c);
            }
        }
        if (s->pfds[1].revents)
            take_returned(s);
        if (s->pfds[0].revents & POLLIN)
            accept_ready(s, &accept_paused_until);
    }
    log_info("shutting down: closing listener and %zu idle connections", s->npoll - 2);
    close(s->listen_fd);
    s->listen_fd = -1;
    for (size_t i = 2; i < s->npoll; i++)
        conn_close(s->pconns[i]);
    s->npoll = 2;
    threadpool_destroy(s->pool); /* in-flight requests finish; workers see g_stop */
    s->pool = NULL;
    take_returned(s);
    log_info("served %llu requests on %llu connections",
             (unsigned long long)atomic_load(&s->stats.requests),
             (unsigned long long)atomic_load(&s->stats.connections));
    return 0;
}

int server_init(server_t *s, config_t *cfg, char *err, size_t err_cap) {
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    s->listen_fd = -1;
    s->wake_pipe[0] = s->wake_pipe[1] = -1;
    s->started_at = now_sec();
    pthread_mutex_init(&s->ret_lock, NULL);

    s->listen_fd = listen_tcp(cfg->bind_addr, cfg->port, LISTEN_BACKLOG, err, err_cap);
    if (s->listen_fd < 0)
        return -1;
    if (pipe(s->wake_pipe) < 0) {
        snprintf(err, err_cap, "pipe: %s", strerror(errno));
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        set_nonblocking(s->wake_pipe[i], 1);
        set_cloexec(s->wake_pipe[i]);
    }
    if (poll_add(s, s->listen_fd, NULL) < 0 || poll_add(s, s->wake_pipe[0], NULL) < 0) {
        snprintf(err, err_cap, "out of memory");
        return -1;
    }
    if (cfg->cache_enabled && cfg->forward_proxy) {
        s->cache = cache_create(cfg->cache_max_bytes, cfg->cache_max_object);
        if (!s->cache) {
            snprintf(err, err_cap, "cache: out of memory");
            return -1;
        }
    }
    /* Workers inherit this mask, so SIGINT/SIGTERM are always delivered to the reactor. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    s->pool = threadpool_create(cfg->threads, (size_t)cfg->max_connections);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (!s->pool) {
        snprintf(err, err_cap, "cannot start %d worker threads", cfg->threads);
        return -1;
    }
    return 0;
}

void server_destroy(server_t *s) {
    if (s->pool)
        threadpool_destroy(s->pool);
    for (size_t i = 2; i < s->npoll; i++)
        conn_close(s->pconns[i]);
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    g_wake_fd = -1;
    for (int i = 0; i < 2; i++)
        if (s->wake_pipe[i] >= 0)
            close(s->wake_pipe[i]);
    cache_destroy(s->cache);
    free(s->pfds);
    free(s->pconns);
    pthread_mutex_destroy(&s->ret_lock);
}
