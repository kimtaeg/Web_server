/*
 * upstream.c — 업스트림 연결/요청 재작성/응답 중계. 설계는 upstream.h 참고.
 *
 * 동기화: 상태를 갖지 않는다. 모든 상태는 호출마다의 지역 변수.
 */
#include "upstream.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "netio.h"
#include "platform.h"

#define RELAY_CHUNK 16384

/* ------------------------------------------------------------------ */
/* 헤더 판별 도우미                                                     */
/* ------------------------------------------------------------------ */

static int ieq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && strncasecmp(s, lit, n) == 0;
}

int upstream_is_hop_by_hop(const char *name, size_t n)
{
    return ieq(name, n, "connection") || ieq(name, n, "keep-alive") || ieq(name, n, "te") ||
           ieq(name, n, "trailer") || ieq(name, n, "transfer-encoding") ||
           ieq(name, n, "upgrade") || (n >= 6 && strncasecmp(name, "proxy-", 6) == 0);
}

int upstream_is_local_error(int err)
{
    return err == EADDRNOTAVAIL || err == EMFILE || err == ENFILE || err == ENOBUFS ||
           err == EAGAIN;
}

/* 쉼표로 구분된 토큰 목록 list 에 name 이 있는가 (대소문자 무시) */
static int token_in_list(const char *list, size_t ll, const char *name, size_t n)
{
    size_t i = 0;

    while (i < ll) {
        size_t a, b;

        while (i < ll && (list[i] == ' ' || list[i] == '\t' || list[i] == ','))
            i++;
        a = i;
        while (i < ll && list[i] != ',')
            i++;
        b = i;
        while (b > a && (list[b - 1] == ' ' || list[b - 1] == '\t'))
            b--;
        if (b - a == n && strncasecmp(list + a, name, n) == 0)
            return 1;
    }
    return 0;
}

/* 요청의 Connection 헤더(여러 개일 수 있음) 중 어디에든 name 이 나열됐는가 */
static int req_named_in_connection(const http_request_t *req, const char *name, size_t n)
{
    size_t i;

    for (i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];

        if (ieq(h->name, h->name_len, "connection") && token_in_list(h->value, h->value_len, name, n))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Via 표식과 루프 감지                                                 */
/* ------------------------------------------------------------------ */

/* 시작 시 한 번만 쓰고 이후 읽기 전용 (upstream.h 참고) */
static const char *g_via = UPSTREAM_VIA_DEFAULT;

void upstream_set_via(const char *via)
{
    g_via = via ? via : UPSTREAM_VIA_DEFAULT;
}

const char *upstream_via(void)
{
    return g_via;
}

int upstream_request_has_our_via(const http_request_t *req)
{
    size_t vl = strlen(g_via);
    size_t i;

    for (i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];

        /* Via 는 쉼표로 구분된 목록. 우리 표식과 정확히 같은 항목이 있는가 */
        if (ieq(h->name, h->name_len, "via") && token_in_list(h->value, h->value_len, g_via, vl))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 요청 재작성                                                          */
/* ------------------------------------------------------------------ */

int upstream_build_request(buf_t *out, const http_request_t *req, const char *target,
                           size_t target_len, const char *host_fallback, const char *client_ip)
{
    char xff[1024] = "";
    char via[1024] = "";
    size_t xl = 0, vl = 0;
    int has_host = 0;
    size_t i;
    int rc;

    rc = buf_appendf(out, "%s %.*s HTTP/1.1\r\n", http_method_name(req->method), (int)target_len,
                     target);
    for (i = 0; rc == BUF_OK && i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];
        const char *nm = h->name;
        size_t nl = h->name_len;

        if (upstream_is_hop_by_hop(nm, nl) || req_named_in_connection(req, nm, nl))
            continue;
        /* 기존 XFF/Via 는 모아 두었다가 우리 값을 덧붙여 한 줄로 다시 쓴다 */
        if (ieq(nm, nl, "x-forwarded-for") || ieq(nm, nl, "via")) {
            char *dst = ieq(nm, nl, "via") ? via : xff;
            size_t *dl = ieq(nm, nl, "via") ? &vl : &xl;
            int w = snprintf(dst + *dl, sizeof xff - *dl, "%s%.*s", *dl ? ", " : "",
                             (int)h->value_len, h->value);

            if (w < 0 || (size_t)w >= sizeof xff - *dl)
                return BUF_ERR_LIMIT; /* 비정상적으로 긴 체인 */
            *dl += (size_t)w;
            continue;
        }
        if (ieq(nm, nl, "expect") || ieq(nm, nl, "x-forwarded-proto") ||
            ieq(nm, nl, "content-length"))
            continue; /* 아래에서 직접 쓴다 */
        if (ieq(nm, nl, "host"))
            has_host = 1;
        rc = buf_appendf(out, "%.*s: %.*s\r\n", (int)nl, nm, (int)h->value_len, h->value);
    }
    if (rc == BUF_OK && !has_host && host_fallback != NULL)
        rc = buf_appendf(out, "Host: %s\r\n", host_fallback);
    if (rc == BUF_OK)
        rc = buf_appendf(out,
                         "X-Forwarded-For: %s%s%s\r\n"
                         "X-Forwarded-Proto: http\r\n"
                         "Via: %s%s%s\r\n",
                         xff, xl ? ", " : "", client_ip ? client_ip : "unknown", via,
                         vl ? ", " : "", g_via);
    if (rc == BUF_OK && (req->body_len > 0 || http_header_get(req, "Content-Length", NULL) != NULL))
        rc = buf_appendf(out, "Content-Length: %zu\r\n", req->body_len);
    if (rc == BUF_OK)
        rc = buf_appendf(out, "Connection: close\r\n\r\n");
    if (rc == BUF_OK && req->body_len > 0)
        rc = buf_append(out, req->body, req->body_len);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 응답 헤더 파싱                                                       */
/* ------------------------------------------------------------------ */

int upstream_parse_response_head(const char *p, size_t n, upstream_resp_t *r)
{
    size_t pos = 0;
    int first = 1;
    int has_te = 0;

    memset(r, 0, sizeof *r);
    for (;;) {
        const char *nl = memchr(p + pos, '\n', n - pos);
        size_t ls = pos, le;

        if (nl == NULL)
            return n > UPSTREAM_MAX_HEAD ? -1 : 1;
        le = (size_t)(nl - p);
        pos = le + 1;
        if (pos > UPSTREAM_MAX_HEAD)
            return -1;
        /* 응답 쪽은 관대하게: LF 만 있는 줄도 허용 (백엔드는 우리가 고른 서버이므로) */
        if (le > ls && p[le - 1] == '\r')
            le--;

        if (first) { /* "HTTP/1.x SSS[ reason]" */
            first = 0;
            if (le - ls < 12 || memcmp(p + ls, "HTTP/1.", 7) != 0 || p[ls + 7] < '0' ||
                p[ls + 7] > '9' || p[ls + 8] != ' ')
                return -1;
            {
                const char *s = p + ls + 9;
                size_t k;

                for (k = 0; k < 3; k++)
                    if (s[k] < '0' || s[k] > '9')
                        return -1;
                if (le - ls > 12 && s[3] != ' ')
                    return -1;
                r->status = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
                if (r->status < 100)
                    return -1;
                r->reason_off = ls + 13 <= le ? ls + 13 : le;
                r->reason_len = le - r->reason_off;
            }
            continue;
        }
        if (le == ls) /* 빈 줄: 헤더 끝 */
            break;
        if (p[ls] == ' ' || p[ls] == '\t')
            return -1; /* obs-fold */
        if (r->nh >= UPSTREAM_MAX_RESP_HEADERS)
            return -1;
        {
            size_t c, vs, ve;

            for (c = ls; c < le && p[c] != ':'; c++)
                if ((unsigned char)p[c] <= ' ' || p[c] == 0x7f)
                    return -1;
            if (c == le || c == ls)
                return -1;
            vs = c + 1;
            ve = le;
            while (vs < ve && (p[vs] == ' ' || p[vs] == '\t'))
                vs++;
            while (ve > vs && (p[ve - 1] == ' ' || p[ve - 1] == '\t'))
                ve--;
            r->name_off[r->nh] = ls;
            r->name_len[r->nh] = c - ls;
            r->val_off[r->nh] = vs;
            r->val_len[r->nh] = ve - vs;

            if (ieq(p + ls, c - ls, "content-length")) {
                size_t v = 0, k;

                if (ve == vs)
                    return -1;
                for (k = vs; k < ve; k++) {
                    if (p[k] < '0' || p[k] > '9' || v > ((size_t)-1 - 9) / 10)
                        return -1;
                    v = v * 10 + (size_t)(p[k] - '0');
                }
                if (r->has_cl && v != r->content_length)
                    return -1; /* 서로 다른 CL: 응답 스머글링 */
                r->has_cl = 1;
                r->content_length = v;
            } else if (ieq(p + ls, c - ls, "transfer-encoding")) {
                has_te = 1;
                /* 마지막 코딩이 chunked 인지 (RFC 9112 6.3) */
                if (ve - vs >= 7 && strncasecmp(p + ve - 7, "chunked", 7) == 0)
                    r->chunked = 1;
                else
                    return -1; /* chunked 로 끝나지 않는 TE 는 길이를 알 수 없음 */
            }
            r->nh++;
        }
    }
    if (has_te) { /* TE 와 CL 이 함께 오면 TE 가 우선, CL 은 무시 (RFC 9112 6.3) */
        r->has_cl = 0;
        r->content_length = 0;
    }
    r->head_len = pos;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 연결                                                                */
/* ------------------------------------------------------------------ */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int upstream_connect(const struct sockaddr *sa, socklen_t len, int timeout_ms)
{
    int fd;
    int fl;
    int rc = UPSTREAM_ECONN;

    fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd < 0)
        return upstream_is_local_error(errno) ? UPSTREAM_ELOCAL : UPSTREAM_ECONN;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || platform_sock_init(fd) != 0)
        goto fail;
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
        goto fail;

    /*
     * blocking connect 는 OS 기본 타임아웃(수십 초~2분)까지 워커를 붙잡는다.
     * non-blocking 으로 시작하고 "쓰기 가능"이 될 때까지 poll 로 기다리면 타임아웃을 직접 정할 수 있다.
     */
    if (connect(fd, sa, len) != 0) {
        long deadline = now_ms() + timeout_ms;
        struct pollfd pfd;
        int soerr = 0;
        socklen_t sl = sizeof soerr;

        if (errno != EINPROGRESS && errno != EINTR) {
            if (upstream_is_local_error(errno))
                rc = UPSTREAM_ELOCAL; /* 임시 포트 고갈 등: 백엔드 탓이 아님 */
            goto fail; /* ECONNREFUSED 등 즉시 실패 */
        }
        pfd.fd = fd;
        pfd.events = POLLOUT;
        for (;;) {
            long left = deadline - now_ms();
            int pr;

            if (left <= 0) {
                rc = UPSTREAM_ETIMEOUT;
                goto fail;
            }
            pfd.revents = 0;
            pr = poll(&pfd, 1, (int)left);
            if (pr > 0)
                break;
            if (pr < 0 && errno != EINTR)
                goto fail;
        }
        /* 쓰기 가능 = 연결 완료 또는 실패. 어느 쪽인지는 SO_ERROR 로 확인 */
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0)
            goto fail;
    }
    if (fcntl(fd, F_SETFL, fl) != 0) /* 이후 I/O 는 blocking + 타임아웃 */
        goto fail;
    {
        int one = 1; /* 요청은 한 번에 만들어 보내므로 Nagle 지연이 필요 없다 */

        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;

fail:
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 응답 중계                                                            */
/* ------------------------------------------------------------------ */

static int resp_named_in_connection(const char *p, const upstream_resp_t *r, const char *name,
                                    size_t n)
{
    size_t i;

    for (i = 0; i < r->nh; i++)
        if (ieq(p + r->name_off[i], r->name_len[i], "connection") &&
            token_in_list(p + r->val_off[i], r->val_len[i], name, n))
            return 1;
    return 0;
}

enum { BODY_NONE, BODY_LENGTH, BODY_UNTIL_EOF };

/* 캐시 사본에 본문 조각을 쌓는다. 상한을 넘으면 그 뒤로는 포기 (중계는 계속) */
static void capture_body(upstream_capture_t *cap, const char *p, size_t n)
{
    if (cap != NULL && !cap->overflow && buf_append(&cap->body, p, n) != BUF_OK)
        cap->overflow = 1;
}

int upstream_relay(int ufd, conn_t *cl, const http_request_t *req, int client_keep_alive,
                   int read_timeout_ms, const char *extra_headers, upstream_capture_t *cap,
                   upstream_result_t *res)
{
    buf_t in;
    buf_t head;
    char tmp[RELAY_CHUNK];
    upstream_resp_t r;
    int rc = UPSTREAM_EBAD;
    int mode = BODY_NONE;
    int ka = client_keep_alive;
    size_t i;
    size_t have;
    size_t left = 0;

    memset(res, 0, sizeof *res);
    buf_init(&in, RELAY_CHUNK, UPSTREAM_MAX_HEAD + RELAY_CHUNK);
    buf_init(&head, 1024, UPSTREAM_MAX_HEAD + 1024);

    /* 1) 응답 헤더를 다 받을 때까지 읽는다. 1xx 중간 응답은 버리고 계속. */
    for (;;) {
        int prc = upstream_parse_response_head(in.data, in.len, &r);
        ssize_t n;

        if (prc == 0) {
            if (r.status >= 100 && r.status < 200 && r.status != 101) {
                buf_consume(&in, r.head_len);
                continue;
            }
            break;
        }
        if (prc < 0)
            goto cleanup; /* EBAD */
        n = recv_some(ufd, tmp, sizeof tmp, read_timeout_ms);
        if (n == NETIO_TIMEOUT) {
            rc = UPSTREAM_ETIMEOUT;
            goto cleanup;
        }
        if (n <= 0 || buf_append(&in, tmp, (size_t)n) != BUF_OK)
            goto cleanup; /* 헤더 전에 EOF/오류 → EBAD */
    }
    if (r.status == 101) /* Upgrade 를 지웠으니 오면 안 된다 */
        goto cleanup;

    /* 2) 본문 구분 방식 결정 */
    if (req->method == HTTP_HEAD || r.status == 204 || r.status == 304) {
        mode = BODY_NONE;
    } else if (r.chunked) {
        if (req->version_minor == 0)
            goto cleanup; /* HTTP/1.0 클라이언트는 chunked 를 이해하지 못한다 → 502 */
        mode = BODY_UNTIL_EOF;
        ka = 0;
    } else if (r.has_cl) {
        mode = BODY_LENGTH;
        left = r.content_length;
    } else {
        mode = BODY_UNTIL_EOF; /* 길이 정보 없음: 업스트림 EOF 가 끝 → 클라이언트도 닫아서 끝을 알린다 */
        ka = 0;
    }

    /* 3) 클라이언트용 헤더 재작성 */
    if (buf_appendf(&head, "HTTP/1.1 %d %.*s\r\n", r.status, (int)r.reason_len,
                    in.data + r.reason_off) != BUF_OK)
        goto cleanup;
    for (i = 0; i < r.nh; i++) {
        const char *nm = in.data + r.name_off[i];
        size_t nl = r.name_len[i];

        if (upstream_is_hop_by_hop(nm, nl) || resp_named_in_connection(in.data, &r, nm, nl))
            continue;
        if (r.chunked && ieq(nm, nl, "content-length"))
            continue;
        if (buf_appendf(&head, "%.*s: %.*s\r\n", (int)nl, nm, (int)r.val_len[i],
                        in.data + r.val_off[i]) != BUF_OK)
            goto cleanup;
    }
    if (r.chunked && buf_appendf(&head, "Transfer-Encoding: chunked\r\n") != BUF_OK)
        goto cleanup;
    if (cap != NULL) { /* 사본에는 연결 단위 헤더(Connection)와 X-Cache 같은 extra 를 넣지 않는다 */
        cap->status = r.status;
        if (buf_append(&cap->head, head.data, head.len) != BUF_OK)
            cap->overflow = 1;
    }
    if ((extra_headers && buf_appendf(&head, "%s", extra_headers) != BUF_OK) ||
        buf_appendf(&head, "Connection: %s\r\n\r\n", ka ? "keep-alive" : "close") != BUF_OK)
        goto cleanup;

    res->status = r.status;
    res->client_keep_alive = ka;
    if (conn_write(cl, head.data, head.len) < 0) {
        rc = UPSTREAM_ECLIENT;
        goto cleanup;
    }
    res->bytes = head.len;
    rc = UPSTREAM_EPARTIAL; /* 이제부터 실패는 "일부 전송 후 실패" */

    /* 4) 본문: 헤더와 함께 이미 받은 바이트부터, 이후는 받는 즉시 중계 */
    have = in.len - r.head_len;
    if (mode == BODY_NONE) {
        rc = UPSTREAM_OK;
        goto cleanup;
    }
    if (mode == BODY_LENGTH && have > left)
        have = left; /* CL 을 넘는 여분은 버린다 */
    if (have > 0) {
        capture_body(cap, in.data + r.head_len, have);
        if (conn_write(cl, in.data + r.head_len, have) < 0) {
            rc = UPSTREAM_ECLIENT;
            goto cleanup;
        }
        res->bytes += have;
        if (mode == BODY_LENGTH)
            left -= have;
    }
    for (;;) {
        ssize_t n;
        size_t want;

        if (mode == BODY_LENGTH && left == 0) {
            rc = UPSTREAM_OK;
            break;
        }
        want = (mode == BODY_LENGTH && left < sizeof tmp) ? left : sizeof tmp;
        n = recv_some(ufd, tmp, want, read_timeout_ms);
        if (n == 0 && mode == BODY_UNTIL_EOF) {
            rc = UPSTREAM_OK;
            break;
        }
        if (n <= 0) /* 타임아웃, 오류, CL 을 채우기 전 EOF → 클라이언트에겐 잘린 응답 */
            break;
        capture_body(cap, tmp, (size_t)n);
        if (conn_write(cl, tmp, (size_t)n) < 0) {
            rc = UPSTREAM_ECLIENT;
            break;
        }
        res->bytes += (size_t)n;
        if (mode == BODY_LENGTH)
            left -= (size_t)n;
    }

cleanup:
    if (rc != UPSTREAM_OK)
        res->client_keep_alive = 0;
    /* 저장해도 되는 사본: 정상 완료 + 길이를 아는 응답(CL) + 잘림 없음 */
    if (cap != NULL)
        cap->complete = rc == UPSTREAM_OK && mode == BODY_LENGTH && !cap->overflow;
    buf_free(&head);
    buf_free(&in);
    return rc;
}
