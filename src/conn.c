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
 * idle_timeout_ms 안에 데이터가 오면 받는다. 한 번에 길게 기다리지 않고 STOP_CHECK_MS 단위로
 * 나눠 기다리며 종료 플래그를 확인한다 → 종료 시 유휴 연결을 기다리느라 늦어지지 않는다.
 * 반환은 recv_some 과 같다 (타임아웃·종료는 NETIO_TIMEOUT).
 */
static ssize_t wait_recv(int fd, char *tmp, size_t n, const conn_config_t *cfg)
{
    long deadline = now_ms() + cfg->idle_timeout_ms;

    for (;;) {
        long left = deadline - now_ms();
        ssize_t r;

        if (left <= 0 || stopping(cfg))
            return NETIO_TIMEOUT;
        r = recv_some(fd, tmp, n, (int)(left < STOP_CHECK_MS ? left : STOP_CHECK_MS));
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
 */
static void linger_close(int fd, char *tmp, size_t n)
{
    size_t drained = 0;

    shutdown(fd, SHUT_WR);
    while (drained < LINGER_MAX) {
        ssize_t r = recv_some(fd, tmp, n, LINGER_MS);

        if (r <= 0)
            break;
        drained += (size_t)r;
    }
    close(fd);
}

void conn_handle(int fd, const conn_config_t *cfg)
{
    char tmp[RECV_CHUNK];
    char ip[INET6_ADDRSTRLEN];
    http_parser_t parser;
    http_request_t req;
    buf_t in;
    int nreq = 0;
    /* 현재 요청의 최대 크기 + 파이프라이닝으로 함께 들어온 다음 요청 조각(최대 recv 1회분) */
    size_t cap = cfg->limits.max_uri + cfg->limits.max_header_bytes + cfg->limits.max_body +
                 2 * RECV_CHUNK;

    peer_ip(fd, ip, sizeof ip);
    if (buf_init(&in, 4096, cap) != BUF_OK)
        goto cleanup;
    if (cfg->send_timeout_ms > 0)
        (void)netio_set_send_timeout(fd, cfg->send_timeout_ms);

    for (;;) {
        parse_result_t r;
        size_t used = 0;
        int ka;
        long t0;
        handler_result_t res;

        /* 버퍼에 이미 다음 요청이 있으면(파이프라이닝) recv 없이 바로 파싱된다. */
        http_parser_init(&parser, &cfg->limits);
        while ((r = http_parse(&parser, in.data, in.len, &req, &used)) == PARSE_NEED_MORE) {
            ssize_t n = wait_recv(fd, tmp, sizeof tmp, cfg);

            if (n <= 0) /* EOF, 오류, 유휴 타임아웃, 종료 */
                goto cleanup;
            if (buf_append(&in, tmp, (size_t)n) != BUF_OK) {
                size_t sent;

                http_response_status_page(fd, 500, 0, 0, NULL, &sent);
                goto cleanup;
            }
        }

        if (r == PARSE_ERROR) {
            int st = http_parser_error_status(&parser);
            size_t sent = 0;

            /* 요청 경계를 잃었으므로 이 연결로는 더 진행할 수 없다 → 항상 close */
            http_response_status_page(fd, st, 0, 0, NULL, &sent);
            log_access(ip, NULL, NULL, 0, 0, st, sent, 0);
            goto cleanup;
        }

        nreq++;
        ka = req.keep_alive && nreq < cfg->max_requests && !stopping(cfg);
        memset(&res, 0, sizeof res);
        t0 = now_ms();
        if (cfg->handler(cfg->handler_ctx, fd, &req, ka, &res) != 0)
            res.close = 1;
        log_access(ip, http_method_name(req.method), req.target, req.target_len, req.version_minor,
                   res.status, res.bytes, now_ms() - t0);
        if (!ka || res.close)
            goto cleanup;
        buf_consume(&in, used); /* 처리한 요청만 덜어내고 남은 바이트(다음 요청)는 보존 */
    }

cleanup:
    buf_free(&in);
    linger_close(fd, tmp, sizeof tmp);
}

void conn_worker(int fd, void *arg)
{
    conn_handle(fd, (const conn_config_t *)arg);
}
