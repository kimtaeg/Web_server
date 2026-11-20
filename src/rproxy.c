/*
 * rproxy.c — 리버스 프록시. 선택·재시도·동기화 규칙은 rproxy.h 참고.
 */
#include "rproxy.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "http_response.h"
#include "log.h"
#include "netio.h"
#include "upstream.h"

long long rproxy_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts); /* 벽시계가 바뀌어도 down 기간이 흔들리지 않게 */
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

int rproxy_pool_init(rproxy_pool_t *p, const config_hostport_t *hps, size_t n, int connect_ms,
                     int read_ms, long down_secs, char *err, size_t errsz)
{
    size_t i;

    memset(p, 0, sizeof *p);
    if (n == 0 || n > CONFIG_MAX_BACKENDS) {
        snprintf(err, errsz, "백엔드 수가 잘못됨 (%zu)", n);
        return -1;
    }
    for (i = 0; i < n; i++) {
        struct addrinfo hints, *ai = NULL;
        char port[16];
        int gai;
        backend_t *b = &p->backends[i];

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port, sizeof port, "%d", hps[i].port);
        gai = getaddrinfo(hps[i].host, port, &hints, &ai);
        if (gai != 0 || ai == NULL) {
            snprintf(err, errsz, "백엔드 '%s:%d' 주소를 풀 수 없음: %s", hps[i].host, hps[i].port,
                     gai_strerror(gai));
            if (ai)
                freeaddrinfo(ai);
            return -1;
        }
        memcpy(&b->addr, ai->ai_addr, ai->ai_addrlen);
        b->addrlen = ai->ai_addrlen;
        freeaddrinfo(ai);
        snprintf(b->name, sizeof b->name, strchr(hps[i].host, ':') ? "[%s]:%d" : "%s:%d",
                 hps[i].host, hps[i].port);
        atomic_init(&b->down_until_ms, 0);
    }
    p->n = n;
    atomic_init(&p->rr, 0);
    p->connect_timeout_ms = connect_ms;
    p->read_timeout_ms = read_ms;
    p->down_ms = (long long)down_secs * 1000LL;
    return 0;
}

int rproxy_pick(rproxy_pool_t *p, long long now_ms, int exclude)
{
    unsigned start = atomic_fetch_add(&p->rr, 1u);
    size_t k;

    for (k = 0; k < p->n; k++) {
        size_t i = (start + k) % p->n;

        if ((int)i == exclude)
            continue;
        if (atomic_load(&p->backends[i].down_until_ms) <= now_ms)
            return (int)i;
    }
    return -1;
}

void rproxy_mark_down(rproxy_pool_t *p, size_t i, long long now_ms)
{
    atomic_store(&p->backends[i].down_until_ms, now_ms + p->down_ms);
}

static int reply(int fd, int status, int ka, int head, handler_result_t *res)
{
    size_t sent = 0;
    int rc = http_response_status_page(fd, status, ka, head, NULL, &sent);

    res->status = status;
    res->bytes = sent;
    return rc;
}

int rproxy_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res)
{
    rproxy_pool_t *p = ctx;
    int is_head = req->method == HTTP_HEAD;
    int idx;
    int attempt;
    int ufd = UPSTREAM_ECONN;
    int rc = 0;
    buf_t out;
    buf_t tgt;
    upstream_result_t ur;
    int relay;

    buf_init(&out, 4096, req->body_len + 128 * 1024);
    buf_init(&tgt, 256, 16 * 1024);

    idx = rproxy_pick(p, rproxy_now_ms(), -1);
    if (idx < 0) {
        log_msg(LOG_WARN, "rproxy: 모든 백엔드가 down → 502");
        rc = reply(fd, 502, keep_alive, is_head, res);
        goto cleanup;
    }
    /* 연결 단계에서만, 최대 1회 재시도 */
    for (attempt = 0; attempt < 2; attempt++) {
        backend_t *b = &p->backends[idx];
        int prev;

        ufd = upstream_connect((const struct sockaddr *)&b->addr, b->addrlen, p->connect_timeout_ms);
        if (ufd >= 0)
            break;
        rproxy_mark_down(p, (size_t)idx, rproxy_now_ms());
        log_msg(LOG_WARN, "rproxy: 백엔드 %s 연결 %s → %llds 동안 down", b->name,
                ufd == UPSTREAM_ETIMEOUT ? "타임아웃" : "실패", p->down_ms / 1000);
        prev = idx;
        idx = rproxy_pick(p, rproxy_now_ms(), prev);
        if (idx < 0)
            break;
    }
    if (ufd < 0) {
        rc = reply(fd, ufd == UPSTREAM_ETIMEOUT ? 504 : 502, keep_alive, is_head, res);
        goto cleanup;
    }

    /* absolute-form 으로 왔으면 업스트림에는 origin-form(path?query)으로 */
    if (req->is_absolute)
        buf_appendf(&tgt, "%.*s%s%.*s", (int)req->path_len, req->path, req->query ? "?" : "",
                    (int)req->query_len, req->query ? req->query : "");
    else
        buf_append(&tgt, req->target, req->target_len);
    if (upstream_build_request(&out, req, tgt.data, tgt.len, p->backends[idx].name,
                               ci ? ci->ip : NULL) != BUF_OK) {
        rc = reply(fd, 500, keep_alive, is_head, res);
        goto cleanup;
    }
    (void)netio_set_send_timeout(ufd, p->read_timeout_ms);
    if (send_all(ufd, out.data, out.len) < 0) {
        log_msg(LOG_WARN, "rproxy: 백엔드 %s 로 요청 전송 실패", p->backends[idx].name);
        rc = reply(fd, 502, keep_alive, is_head, res);
        goto cleanup;
    }

    relay = upstream_relay(ufd, fd, req, keep_alive, p->read_timeout_ms, &ur);
    switch (relay) {
    case UPSTREAM_OK:
        res->status = ur.status;
        res->bytes = ur.bytes;
        res->close = !ur.client_keep_alive;
        break;
    case UPSTREAM_ETIMEOUT:
        log_msg(LOG_WARN, "rproxy: 백엔드 %s 응답 타임아웃 → 504", p->backends[idx].name);
        rc = reply(fd, 504, keep_alive, is_head, res);
        break;
    case UPSTREAM_EBAD:
        log_msg(LOG_WARN, "rproxy: 백엔드 %s 응답 형식 오류 → 502", p->backends[idx].name);
        rc = reply(fd, 502, keep_alive, is_head, res);
        break;
    default: /* EPARTIAL / ECLIENT: 응답이 이미 나가는 중이었다 → 연결을 끊어 잘렸음을 알린다 */
        res->status = ur.status;
        res->bytes = ur.bytes;
        res->close = 1;
        break;
    }

cleanup:
    if (ufd >= 0)
        close(ufd);
    buf_free(&tgt);
    buf_free(&out);
    return rc;
}
