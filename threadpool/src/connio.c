/*
 * connio.c — 평문/TLS 공통 클라이언트 I/O. 설계는 connio.h 참고.
 */
#include "connio.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "netio.h"
#include "tls.h"

void conn_init_plain(conn_t *c, int fd)
{
    c->fd = fd;
    c->ssl = NULL;
    c->tls_broken = 0;
}

ssize_t conn_write(conn_t *c, const void *p, size_t n)
{
    const char *s = p;
    size_t off = 0;

    if (c->ssl == NULL)
        return send_all(c->fd, p, n); /* 기존 HTTP 경로 그대로 */
    if (c->tls_broken)
        return -1;
    /*
     * SSL_write 는 int 길이를 받고, 레코드(최대 16KB) 단위로 암호화해 보낸다.
     * SSL_MODE_ENABLE_PARTIAL_WRITE 를 켜지 않았으므로 blocking 소켓에서는 보통 전부 쓰고 돌아오지만,
     * INT_MAX 보다 긴 입력과 부분 쓰기 모드 변경에 대비해 남은 만큼 반복한다.
     */
    while (off < n) {
        size_t chunk = n - off < (size_t)INT_MAX ? n - off : (size_t)INT_MAX;
        int w;

        ERR_clear_error();
        w = SSL_write(c->ssl, s + off, (int)chunk);
        if (w <= 0) {
            (void)tls_io_fail(c, "SSL_write", w);
            c->tls_broken = 1; /* 쓰다 만 레코드가 있을 수 있다: close_notify 를 보내지 않는다 */
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)n;
}

ssize_t conn_read(conn_t *c, void *p, size_t n, int timeout_ms)
{
    int chunk = n < (size_t)INT_MAX ? (int)n : INT_MAX;
    int r;

    if (c->ssl == NULL)
        return recv_some(c->fd, p, n, timeout_ms);
    if (c->tls_broken)
        return -1;

    /*
     * 핵심: OpenSSL 은 소켓에서 레코드 단위로 읽어 복호화하고, 요청한 것보다 많이 풀린 평문은 내부에
     * 남겨 둔다 (예: 파이프라이닝된 요청 두 개가 한 레코드에 옴). 그 바이트는 커널 버퍼에 없으므로
     * poll 은 "읽을 것 없음"이라고 답한다 → SSL_pending 이 0 일 때만 poll 로 기다린다.
     */
    if (SSL_pending(c->ssl) == 0) {
        struct pollfd pfd;
        int pr;

        pfd.fd = c->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0)
            return NETIO_TIMEOUT;
        if (pr < 0)
            return errno == EINTR ? NETIO_TIMEOUT : -1; /* EINTR: 호출자가 마감을 다시 계산해 재호출 */
    }
    ERR_clear_error();
    r = SSL_read(c->ssl, p, chunk);
    if (r > 0)
        return r;
    switch (tls_io_fail(c, "SSL_read", r)) {
    case TLS_IO_EOF: return 0;
    case TLS_IO_TIMEOUT: return NETIO_TIMEOUT; /* SSL_read 는 다른 버퍼로 재시도해도 된다 */
    default: return -1;
    }
}
