/*
 * conn.c — keep-alive 루프. 설계는 conn.h 참고.
 */
#include "conn.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "http_response.h"
#include "log.h"
#include "netio.h"
#include "tls.h"

#define RECV_CHUNK 16384
#define STOP_CHECK_MS 250   /* 대기 중 종료 플래그 확인 주기 */
#define LINGER_MS 200       /* 닫기 전 남은 입력을 비우는 최대 대기 */
#define LINGER_MAX (64 * 1024)

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int stopping(const conn_config_t *cfg)
{
    return cfg->stop != NULL && *cfg->stop;
}

/*
 * timeout_ms 안에 데이터가 오면 받는다. 한 번에 길게 기다리지 않고 STOP_CHECK_MS 단위로
 * 나눠 기다리며 종료 플래그를 확인한다 → 종료 시 유휴 연결을 기다리느라 늦어지지 않는다.
 * 반환은 recv_some 과 같다 (타임아웃·종료는 NETIO_TIMEOUT). TLS 면 conn_read 가 SSL_read 로 읽는다.
 */
static ssize_t wait_recv(conn_t *c, char *tmp, size_t n, const conn_config_t *cfg, long timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        long left = deadline - now_ms();
        ssize_t r;

        if (left <= 0 || stopping(cfg))
            return NETIO_TIMEOUT;
        r = conn_read(c, tmp, n, (int)(left < STOP_CHECK_MS ? left : STOP_CHECK_MS));
        if (r != NETIO_TIMEOUT)
            return r;
    }
}

/* 접근 로그용 상대 IP. IPv4/IPv6 가 아니면(테스트의 socketpair 등) "-". */
static void peer_ip(int fd, char *out, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    const void *addr = NULL;

    snprintf(out, n, "-");
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
        return;
    if (ss.ss_family == AF_INET)
        addr = &((struct sockaddr_in *)&ss)->sin_addr;
    else if (ss.ss_family == AF_INET6)
        addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
    if (addr == NULL || inet_ntop(ss.ss_family, addr, out, (socklen_t)n) == NULL)
        snprintf(out, n, "-");
}

/*
 * 바로 close 하면, 아직 읽지 않은 입력이 커널 버퍼에 남아 있을 때 TCP 가 RST 를 보낸다.
 * 그러면 클라이언트가 직전에 보낸 응답(예: 400)을 읽기 전에 연결 리셋으로 버릴 수 있다.
 * 쓰기 방향만 먼저 닫고(FIN), 남은 입력을 잠깐 비운 뒤 닫는다.
 *
 * TLS 면 그 전에 tls_close 로 close_notify 를 보내고 SSL 객체를 해제한다 (상대에게 "응답이 잘린 게
 * 아니라 정상 종료"라고 알림). 이후 비우는 바이트는 복호화할 필요 없는 암호문이라 소켓에서 직접 읽는다.
 */
static void linger_close(conn_t *c, char *tmp, size_t n)
{
    size_t drained = 0;
    int fd = c->fd;

    tls_close(c);
    shutdown(fd, SHUT_WR);
    while (drained < LINGER_MAX) {
        ssize_t r = recv_some(fd, tmp, n, LINGER_MS);

        if (r <= 0)
            break;
        drained += (size_t)r;
    }
    close(fd);
}

/*
 * HTTPS 포트에 평문 HTTP 가 왔을 때: 핸드셰이크 전이라 암호화할 수 없으므로 평문 400 을 보낸다.
 * (브라우저/curl 이 "http://...:8443" 으로 잘못 접속한 경우 원인을 바로 알 수 있게. nginx 와 같은 방식)
 */
static void reply_plain_http_on_tls(conn_t *c, const char *ip)
{
    size_t sent = 0;

    (void)http_response_status_page(c, 400, 0, 0, "X-Error: plain HTTP request sent to HTTPS port\r\n",
                                    &sent);
    log_access(ip, NULL, NULL, 0, 0, 400, sent, 0);
}

void conn_handle(int fd, int tls, const conn_config_t *cfg)
{
    conn_t c;
    char tmp[RECV_CHUNK];
    char ip[INET6_ADDRSTRLEN];
    conn_info_t ci;
    http_parser_t parser;
    http_request_t req;
    buf_t in;
    int nreq = 0;
    /* 현재 요청의 최대 크기 + 파이프라이닝으로 함께 들어온 다음 요청 조각(최대 recv 1회분) */
    size_t cap = cfg->limits.max_uri + cfg->limits.max_header_bytes + cfg->limits.max_body +
                 2 * RECV_CHUNK;

    conn_init_plain(&c, fd);
    peer_ip(fd, ip, sizeof ip);
    ci.ip = ip;
    if (buf_init(&in, 4096, cap) != BUF_OK)
        goto cleanup;
    if (cfg->send_timeout_ms > 0)
        (void)netio_set_send_timeout(fd, cfg->send_timeout_ms);
    if (tls) {
        int hs_ms = cfg->header_timeout_ms > 0 ? cfg->header_timeout_ms : cfg->idle_timeout_ms;
        int hr;

        if (cfg->tls_ctx == NULL)
            goto cleanup;
        hr = tls_accept(cfg->tls_ctx, &c, hs_ms, cfg->stop);
        if (hr == TLS_PLAIN_HTTP)
            reply_plain_http_on_tls(&c, ip);
        if (hr != TLS_OK)
            goto cleanup; /* SSL 은 tls_accept 가 이미 해제했다. 소켓은 아래에서 닫는다. */
        /* OpenSSL 이 소켓을 직접 read 하므로, 레코드 일부만 온 채 멈춘 상대에게 영원히 블록하지 않게 */
        (void)netio_set_recv_timeout(fd, cfg->idle_timeout_ms);
    }

    for (;;) {
        parse_result_t r;
        size_t used = 0;
        int ka;
        long t0;
        long req_start;
        handler_result_t res;

        /* 버퍼에 이미 다음 요청이 있으면(파이프라이닝) recv 없이 바로 파싱된다. */
        http_parser_init(&parser, &cfg->limits);
        req_start = in.len > 0 ? now_ms() : -1; /* 이 요청의 첫 바이트가 도착한 시각 */
        while ((r = http_parse(&parser, in.data, in.len, &req, &used)) == PARSE_NEED_MORE) {
            long wait = cfg->idle_timeout_ms;
            int header_clock = req_start >= 0 && cfg->header_timeout_ms > 0 &&
                               !http_parser_headers_done(&parser);
            ssize_t n;

            if (header_clock) { /* 헤더 마감까지 남은 시간이 더 짧으면 그만큼만 기다린다 */
                long left = req_start + cfg->header_timeout_ms - now_ms();

                if (left < wait)
                    wait = left;
            }
            n = wait > 0 ? wait_recv(&c, tmp, sizeof tmp, cfg, wait) : NETIO_TIMEOUT;
            if (n == NETIO_TIMEOUT && header_clock && !stopping(cfg) &&
                now_ms() - req_start >= cfg->header_timeout_ms) {
                size_t sent = 0;

                /* slowloris: 헤더를 마감 안에 다 보내지 못함 → 408 후 종료 */
                http_response_status_page(&c, 408, 0, 0, NULL, &sent);
                log_access(ip, NULL, NULL, 0, 0, 408, sent, now_ms() - req_start);
                goto cleanup;
            }
            if (n <= 0) /* EOF, 오류, 유휴 타임아웃, 종료 */
                goto cleanup;
            if (req_start < 0)
                req_start = now_ms();
            if (buf_append(&in, tmp, (size_t)n) != BUF_OK) {
                size_t sent;

                http_response_status_page(&c, 500, 0, 0, NULL, &sent);
                goto cleanup;
            }
        }

        if (r == PARSE_ERROR) {
            int st = http_parser_error_status(&parser);
            size_t sent = 0;

            /* 요청 경계를 잃었으므로 이 연결로는 더 진행할 수 없다 → 항상 close */
            http_response_status_page(&c, st, 0, 0, NULL, &sent);
            log_access(ip, NULL, NULL, 0, 0, st, sent, 0);
            goto cleanup;
        }

        nreq++;
        ka = req.keep_alive && nreq < cfg->max_requests && !stopping(cfg);
        memset(&res, 0, sizeof res);
        t0 = now_ms();
        if (cfg->handler(cfg->handler_ctx, &c, &req, &ci, ka, &res) != 0)
            res.close = 1;
        log_access(ip, http_method_name(req.method), req.target, req.target_len, req.version_minor,
                   res.status, res.bytes, now_ms() - t0);
        if (!ka || res.close)
            goto cleanup;
        buf_consume(&in, used); /* 처리한 요청만 덜어내고 남은 바이트(다음 요청)는 보존 */
    }

cleanup:
    buf_free(&in);
    linger_close(&c, tmp, sizeof tmp);
}

void conn_worker(threadpool_item_t item, void *arg)
{
    conn_handle(item.fd, item.tls, (const conn_config_t *)arg);
}
