/*
 * fproxy.c — 포워드 프록시 + 캐시. 저장 조건·TTL·보안은 fproxy.h 참고.
 *
 * 동기화: fproxy_ctx_t 는 시작 시 한 번 쓰고 읽기 전용. 캐시 자체의 동기화는 cache.h.
 */
#include "fproxy.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "http_response.h"
#include "log.h"
#include "netio.h"
#include "upstream.h"

#define FPROXY_KEY_MAX 8400 /* 대상 상한(8KB) + 여유 */

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static int ieq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && strncasecmp(s, lit, n) == 0;
}

/* ------------------------------------------------------------------ */
/* 순수 함수                                                           */
/* ------------------------------------------------------------------ */

int fproxy_split_authority(const char *a, size_t n, char *host, size_t hostsz, int *port)
{
    size_t hs = 0, he, ps;
    long p = 80;

    if (n == 0)
        return -1;
    if (a[0] == '[') { /* [IPv6]:port */
        const char *rb = memchr(a, ']', n);

        if (rb == NULL)
            return -1;
        hs = 1;
        he = (size_t)(rb - a);
        ps = he + 1;
        if (ps < n && a[ps] != ':')
            return -1;
    } else {
        const char *colon = memchr(a, ':', n);

        he = colon ? (size_t)(colon - a) : n;
        ps = he;
    }
    if (ps < n) { /* ":port" */
        size_t i;

        if (ps + 1 >= n)
            return -1;
        p = 0;
        for (i = ps + 1; i < n; i++) {
            if (a[i] < '0' || a[i] > '9')
                return -1;
            p = p * 10 + (a[i] - '0');
            if (p > 65535)
                return -1;
        }
        if (p == 0)
            return -1;
    }
    if (he <= hs || he - hs >= hostsz)
        return -1;
    memcpy(host, a + hs, he - hs);
    host[he - hs] = '\0';
    *port = (int)p;
    return 0;
}

int fproxy_cache_key(const http_request_t *req, char *out, size_t outsz)
{
    char host[256];
    int port;
    size_t i;
    int n;
    int v6;

    if (!req->is_absolute ||
        fproxy_split_authority(req->authority, req->authority_len, host, sizeof host, &port) != 0)
        return -1;
    for (i = 0; host[i]; i++)
        host[i] = (char)tolower((unsigned char)host[i]); /* 호스트는 대소문자 무관 (RFC 3986 6.2.2.1) */
    v6 = strchr(host, ':') != NULL;
    if (port == 80)
        n = snprintf(out, outsz, "http://%s%s%s%.*s%s%.*s", v6 ? "[" : "", host, v6 ? "]" : "",
                     (int)req->path_len, req->path, req->query ? "?" : "", (int)req->query_len,
                     req->query ? req->query : "");
    else
        n = snprintf(out, outsz, "http://%s%s%s:%d%.*s%s%.*s", v6 ? "[" : "", host, v6 ? "]" : "",
                     port, (int)req->path_len, req->path, req->query ? "?" : "",
                     (int)req->query_len, req->query ? req->query : "");
    if (n < 0 || (size_t)n >= outsz)
        return -1;
    return n;
}

/* Cache-Control 값에서 지시어 하나를 찾는다. "max-age=60" 같은 값은 *val 로 (-1 = 값 없음) */
static int cc_has(const char *v, size_t n, const char *name, long long *val)
{
    size_t i = 0, nl = strlen(name);

    while (i < n) {
        size_t a, b;

        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ','))
            i++;
        a = i;
        while (i < n && v[i] != ',')
            i++;
        b = i;
        while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t'))
            b--;
        if (b - a >= nl && strncasecmp(v + a, name, nl) == 0 &&
            (b - a == nl || v[a + nl] == '=')) {
            if (val != NULL) {
                *val = -1;
                if (b - a > nl + 1) {
                    long long x = 0;
                    size_t k;

                    for (k = a + nl + 1; k < b && v[k] >= '0' && v[k] <= '9'; k++)
                        if (x < 1000000000LL)
                            x = x * 10 + (v[k] - '0');
                    if (k == b)
                        *val = x;
                }
            }
            return 1;
        }
    }
    return 0;
}

int fproxy_cacheable(const http_request_t *req, const char *head, size_t head_len,
                     long long default_ttl_ms, long long *ttl_ms)
{
    buf_t tmp;
    upstream_resp_t r;
    long long maxage = -1, smaxage = -1, v;
    size_t i;
    int ok = 0;

    if (req->method != HTTP_GET || http_header_get(req, "Authorization", NULL) != NULL)
        return 0;
    /* 저장해 둔 head 를 응답 파서로 다시 읽는다 (빈 줄을 붙여서) */
    buf_init(&tmp, head_len + 4, head_len + 4);
    if (buf_append(&tmp, head, head_len) != BUF_OK || buf_append(&tmp, "\r\n", 2) != BUF_OK ||
        upstream_parse_response_head(tmp.data, tmp.len, &r) != 0)
        goto done;
    if (r.status != 200 || !r.has_cl)
        goto done;
    for (i = 0; i < r.nh; i++) {
        const char *nm = tmp.data + r.name_off[i];
        const char *vl = tmp.data + r.val_off[i];
        size_t nn = r.name_len[i], vn = r.val_len[i];

        if (ieq(nm, nn, "set-cookie"))
            goto done;
        if (ieq(nm, nn, "cache-control")) {
            if (cc_has(vl, vn, "no-store", NULL) || cc_has(vl, vn, "private", NULL) ||
                cc_has(vl, vn, "no-cache", NULL))
                goto done;
            if (cc_has(vl, vn, "s-maxage", &v) && v >= 0)
                smaxage = v;
            if (cc_has(vl, vn, "max-age", &v) && v >= 0)
                maxage = v;
        }
    }
    /* 공유 캐시는 s-maxage 가 max-age 보다 우선 (RFC 9111 5.2.2.10) */
    *ttl_ms = smaxage >= 0 ? smaxage * 1000 : maxage >= 0 ? maxage * 1000 : default_ttl_ms;
    ok = *ttl_ms > 0;

done:
    buf_free(&tmp);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 핸들러                                                              */
/* ------------------------------------------------------------------ */

static int reply(int fd, int status, int ka, int head, const char *extra, handler_result_t *res)
{
    size_t sent = 0;
    int rc = http_response_status_page(fd, status, ka, head, extra, &sent);

    res->status = status;
    res->bytes = sent;
    return rc;
}

/* 캐시 HIT 응답: 저장된 head + X-Cache/Connection + (HEAD 가 아니면) 본문. send 한 번. */
static int serve_hit(int fd, const http_request_t *req, const buf_t *blob, size_t head_len, int ka,
                     handler_result_t *res)
{
    buf_t out;
    int rc = 0;

    buf_init(&out, blob->len + 128, blob->len + 256);
    if (buf_append(&out, blob->data, head_len) != BUF_OK ||
        buf_appendf(&out, "X-Cache: HIT\r\nConnection: %s\r\n\r\n", ka ? "keep-alive" : "close") !=
            BUF_OK ||
        (req->method != HTTP_HEAD &&
         buf_append(&out, blob->data + head_len, blob->len - head_len) != BUF_OK)) {
        buf_free(&out);
        return reply(fd, 500, ka, req->method == HTTP_HEAD, NULL, res);
    }
    res->status = 200;
    if (send_all(fd, out.data, out.len) < 0)
        rc = -1;
    res->bytes = out.len;
    buf_free(&out);
    return rc;
}

/* getaddrinfo 결과를 순서대로 시도. 전부 실패하면 마지막 오류(UPSTREAM_ECONN/ETIMEOUT). */
static int connect_any(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *ai = NULL, *p;
    char ps[16];
    int fd = UPSTREAM_ECONN;
    int gai;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    gai = getaddrinfo(host, ps, &hints, &ai);
    if (gai != 0) {
        log_msg(LOG_WARN, "fproxy: '%s' 주소 해석 실패: %s", host, gai_strerror(gai));
        return UPSTREAM_ECONN;
    }
    /* "localhost" → ::1, 127.0.0.1 처럼 여러 주소가 나온다. 대상이 IPv4 에서만 듣고 있으면
     * 첫 주소(::1)는 거부되므로 다음 주소로 넘어가야 한다. */
    for (p = ai; p != NULL; p = p->ai_next) {
        fd = upstream_connect(p->ai_addr, p->ai_addrlen, timeout_ms);
        if (fd >= 0)
            break;
    }
    freeaddrinfo(ai);
    return fd;
}

int fproxy_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res)
{
    const fproxy_ctx_t *f = ctx;
    int is_head = req->method == HTTP_HEAD;
    char key[FPROXY_KEY_MAX];
    char host[256];
    int port;
    int klen;
    int ufd = -1;
    int rc = 0;
    int use_cache;
    const char *miss;
    buf_t blob, out, tgt;
    upstream_capture_t cap;
    upstream_result_t ur;
    int relay;

    buf_init(&blob, 0, 64 * 1024 * 1024);
    buf_init(&out, 4096, req->body_len + 128 * 1024);
    buf_init(&tgt, 256, 16 * 1024);
    memset(&cap, 0, sizeof cap);

    if (req->is_https) {
        rc = reply(fd, 501, keep_alive, is_head, NULL, res); /* TLS 터널링 미지원 */
        goto cleanup;
    }
    if (upstream_request_has_our_via(req)) {
        log_msg(LOG_WARN, "fproxy: 프록시 루프 감지 (Via 에 자기 표식) → 508");
        rc = reply(fd, 508, 0, is_head, NULL, res);
        res->close = 1;
        goto cleanup;
    }
    klen = fproxy_cache_key(req, key, sizeof key);
    if (klen < 0 ||
        fproxy_split_authority(req->authority, req->authority_len, host, sizeof host, &port) != 0) {
        rc = reply(fd, 400, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    use_cache = f->cache != NULL && (req->method == HTTP_GET || req->method == HTTP_HEAD) &&
                http_header_get(req, "Authorization", NULL) == NULL;
    /* 캐시가 켜져 있으면 HIT 이 아닌 모든 응답은 MISS (캐시를 건너뛴 POST/인증 요청 포함) */
    miss = f->cache != NULL ? "X-Cache: MISS\r\n" : NULL;

    /* 1) 캐시 조회 */
    if (use_cache) {
        size_t head_len = 0;

        if (cache_get(f->cache, key, (size_t)klen, now_ms(), &blob, &head_len) == CACHE_HIT) {
            rc = serve_hit(fd, req, &blob, head_len, keep_alive, res);
            goto cleanup;
        }
    }

    /* 2) 대상에 연결 */
    ufd = connect_any(host, port, f->connect_timeout_ms);
    if (ufd < 0) {
        rc = reply(fd, ufd == UPSTREAM_ETIMEOUT ? 504 : 502, keep_alive, is_head, miss, res);
        goto cleanup;
    }

    /* 3) origin-form 으로 재작성해서 전송 */
    buf_appendf(&tgt, "%.*s%s%.*s", (int)req->path_len, req->path, req->query ? "?" : "",
                (int)req->query_len, req->query ? req->query : "");
    if (upstream_build_request(&out, req, tgt.data, tgt.len, NULL, ci ? ci->ip : NULL) != BUF_OK) {
        rc = reply(fd, 500, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    (void)netio_set_send_timeout(ufd, f->read_timeout_ms);
    if (send_all(ufd, out.data, out.len) < 0) {
        rc = reply(fd, 502, keep_alive, is_head, NULL, res);
        goto cleanup;
    }

    /* 4) 중계 + (GET 이면) 사본 */
    if (use_cache && req->method == HTTP_GET) {
        buf_init(&cap.head, 1024, UPSTREAM_MAX_HEAD);
        buf_init(&cap.body, 0, 64 * 1024 * 1024); /* 실제 상한은 캐시의 max_object 가 거른다 */
    }
    relay = upstream_relay(ufd, fd, req, keep_alive, f->read_timeout_ms, miss,
                           (use_cache && req->method == HTTP_GET) ? &cap : NULL, &ur);
    switch (relay) {
    case UPSTREAM_OK:
        res->status = ur.status;
        res->bytes = ur.bytes;
        res->close = !ur.client_keep_alive;
        break;
    case UPSTREAM_ETIMEOUT:
        rc = reply(fd, 504, keep_alive, is_head, NULL, res);
        break;
    case UPSTREAM_EBAD:
        rc = reply(fd, 502, keep_alive, is_head, NULL, res);
        break;
    default:
        res->status = ur.status;
        res->bytes = ur.bytes;
        res->close = 1;
        break;
    }

    /* 5) 저장: 사본이 완전하고 저장 조건을 만족할 때만 */
    if (relay == UPSTREAM_OK && cap.complete) {
        long long ttl = 0;

        if (fproxy_cacheable(req, cap.head.data, cap.head.len, f->default_ttl_ms, &ttl)) {
            buf_t val;

            /* 값 = head + body, aux = head 길이 */
            buf_init(&val, cap.head.len + cap.body.len, cap.head.len + cap.body.len + 1);
            if (buf_append(&val, cap.head.data, cap.head.len) == BUF_OK &&
                buf_append(&val, cap.body.data ? cap.body.data : "", cap.body.len) == BUF_OK)
                (void)cache_put(f->cache, key, (size_t)klen, val.data, val.len, cap.head.len,
                                now_ms() + ttl);
            buf_free(&val);
        }
    }

cleanup:
    if (ufd >= 0)
        close(ufd);
    buf_free(&cap.body);
    buf_free(&cap.head);
    buf_free(&tgt);
    buf_free(&out);
    buf_free(&blob);
    return rc;
}
