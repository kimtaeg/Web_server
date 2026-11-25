/*
 * tls.c — OpenSSL 로 서버 쪽 TLS. 설계와 동기화는 tls.h 참고.
 */
#include "tls.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "netio.h"

#define HANDSHAKE_POLL_MS 250 /* 핸드셰이크 대기 중 종료 플래그 확인 주기 */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ERR_print_errors_cb 콜백: 오류 한 줄을 로그 한 줄로. str 끝의 '\n' 은 뗀다. */
static int log_err_line(const char *str, size_t len, void *u)
{
    const log_level_t *lvl = u;

    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r'))
        len--;
    log_msg(*lvl, "  openssl: %.*s", (int)len, str);
    return 1; /* 계속 */
}

/*
 * ERR_print_errors_fp(stderr) 대신 콜백으로 log_msg 를 거친다: log 모듈은 한 줄을 락 안에서 쓰므로
 * 여러 워커의 오류 줄이 서로 섞이지 않는다. 출력 후 큐는 비워진다.
 */
void tls_log_queue(log_level_t lvl)
{
    ERR_print_errors_cb(log_err_line, &lvl);
}

static const char *ssl_err_name(int e)
{
    switch (e) {
    case SSL_ERROR_NONE: return "SSL_ERROR_NONE";
    case SSL_ERROR_ZERO_RETURN: return "SSL_ERROR_ZERO_RETURN";
    case SSL_ERROR_WANT_READ: return "SSL_ERROR_WANT_READ";
    case SSL_ERROR_WANT_WRITE: return "SSL_ERROR_WANT_WRITE";
    case SSL_ERROR_SYSCALL: return "SSL_ERROR_SYSCALL";
    case SSL_ERROR_SSL: return "SSL_ERROR_SSL";
    default: return "SSL_ERROR_?";
    }
}

SSL_CTX *tls_ctx_create(const char *cert_path, const char *key_path)
{
    SSL_CTX *ctx = NULL;

    /* 1.1.0 부터는 자동 초기화되지만, 명시적으로 불러 실패를 여기서 잡는다. 여러 번 불러도 안전. */
    if (OPENSSL_init_ssl(0, NULL) != 1) {
        log_msg(LOG_ERROR, "tls: OpenSSL 초기화 실패");
        goto fail;
    }
    ctx = SSL_CTX_new(TLS_server_method()); /* 버전 협상형 메서드: 하한은 아래에서 건다 */
    if (ctx == NULL) {
        log_msg(LOG_ERROR, "tls: SSL_CTX_new 실패");
        goto fail;
    }
    /* TLS 1.0/1.1 은 폐기됨 (RFC 8996). 1.2 이상만 협상한다. */
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        log_msg(LOG_ERROR, "tls: 최소 버전(TLS 1.2) 설정 실패");
        goto fail;
    }
    /* chain_file: 서버 인증서 뒤에 중간 인증서가 이어 붙어 있어도 함께 보낸다 (자체 서명이면 1장) */
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        log_msg(LOG_ERROR, "tls: 인증서 '%s' 를 읽을 수 없음 (PEM 형식인지, 경로가 맞는지 확인)",
                cert_path);
        goto fail;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        log_msg(LOG_ERROR, "tls: 개인키 '%s' 를 읽을 수 없거나 인증서와 짝이 아님", key_path);
        goto fail;
    }
    /* 인증서의 공개키와 개인키가 한 쌍인지. 다르면 핸드셰이크가 매번 실패하므로 시작 시 거부한다. */
    if (SSL_CTX_check_private_key(ctx) != 1) {
        log_msg(LOG_ERROR, "tls: 인증서 '%s' 와 개인키 '%s' 가 짝이 아님", cert_path, key_path);
        goto fail;
    }
    /* blocking 소켓: 비데이터 레코드(TLS 1.3 세션 티켓 등) 처리 후 SSL_read 가 알아서 재시도 */
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
#ifdef SSL_OP_NO_RENEGOTIATION
    /* 클라이언트 주도 재협상은 서버 CPU 를 소모시키는 DoS 수단이 될 수 있다. HTTP/1.1 에는 필요 없다. */
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
#endif
    return ctx;

fail:
    tls_log_queue(LOG_ERROR);
    SSL_CTX_free(ctx); /* NULL 이면 아무것도 안 한다 */
    return NULL;
}

void tls_ctx_free(SSL_CTX *ctx)
{
    SSL_CTX_free(ctx);
}

/* 핸드셰이크 실패 직후 오류 큐 맨 앞이 "평문 HTTP 요청"인지 (OpenSSL 이 "GET " 등을 감지) */
static int is_plain_http(void)
{
    unsigned long e = ERR_peek_error();

    return e != 0 && ERR_GET_LIB(e) == ERR_LIB_SSL && ERR_GET_REASON(e) == SSL_R_HTTP_REQUEST;
}

int tls_accept(SSL_CTX *ctx, conn_t *c, int timeout_ms, volatile sig_atomic_t *stop)
{
    SSL *ssl = NULL;
    long deadline = now_ms() + timeout_ms;
    int fl = -1;
    int rc = TLS_FAIL;

    ERR_clear_error();
    ssl = SSL_new(ctx); /* 연결별 상태(키, 시퀀스 번호, 버퍼)를 담는 객체. 워커 하나만 쓴다. */
    if (ssl == NULL) {
        log_msg(LOG_ERROR, "tls: SSL_new 실패");
        tls_log_queue(LOG_ERROR);
        goto cleanup;
    }
    /* 기존 TCP 소켓을 그대로 넘긴다. OpenSSL 이 이 fd 로 암호화된 레코드를 read/write 한다.
     * (소켓 BIO 는 BIO_NOCLOSE: SSL_free 가 fd 를 닫지 않는다 → 소켓 소유권은 그대로 conn) */
    if (SSL_set_fd(ssl, c->fd) != 1) {
        log_msg(LOG_ERROR, "tls: SSL_set_fd 실패");
        tls_log_queue(LOG_ERROR);
        goto cleanup;
    }
    /* 핸드셰이크 동안만 non-blocking: WANT_READ/WANT_WRITE 를 받아 poll 로 마감까지 기다린다 */
    fl = fcntl(c->fd, F_GETFL);
    if (fl < 0 || fcntl(c->fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        fl = -1;
        goto cleanup;
    }

    for (;;) {
        struct pollfd pfd;
        long left;
        int r, e, pr;

        ERR_clear_error(); /* SSL_get_error 가 이전 호출의 잔여 오류에 속지 않도록 */
        r = SSL_accept(ssl);
        if (r == 1) {
            rc = TLS_OK;
            break;
        }
        e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            int saved = errno;

            if (is_plain_http()) {
                log_msg(LOG_WARN, "tls: HTTPS 포트에 평문 HTTP 요청 → 400 후 종료");
                ERR_clear_error();
                rc = TLS_PLAIN_HTTP;
            } else if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {
                /* 오류 큐가 비어 있는 SYSCALL = 핸드셰이크 도중 상대가 끊음 (errno 0 이면 EOF) */
                log_msg(LOG_WARN, "tls: 핸드셰이크 중 연결 끊김 (%s, errno %d%s%s)",
                        ssl_err_name(e), saved, saved ? ": " : "", saved ? strerror(saved) : "");
            } else {
                /* 버전·암호 불일치, 상대가 보낸 alert(예: 인증서 거부 'unknown ca') 등 */
                log_msg(LOG_WARN, "tls: 핸드셰이크 실패 (%s)", ssl_err_name(e));
                tls_log_queue(LOG_WARN);
            }
            break;
        }
        left = deadline - now_ms();
        if (left <= 0 || (stop != NULL && *stop)) {
            log_msg(LOG_WARN, "tls: 핸드셰이크 마감(%dms) 초과 또는 종료 중 → 연결 종료", timeout_ms);
            rc = TLS_TIMEOUT;
            break;
        }
        pfd.fd = c->fd;
        pfd.events = e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        pfd.revents = 0;
        pr = poll(&pfd, 1, (int)(left < HANDSHAKE_POLL_MS ? left : HANDSHAKE_POLL_MS));
        if (pr < 0 && errno != EINTR) {
            log_msg(LOG_WARN, "tls: 핸드셰이크 poll 실패: %s", strerror(errno));
            break;
        }
        /* pr == 0 (아직 안 옴) / > 0 (준비됨) / EINTR: 모두 SSL_accept 를 다시 불러 본다 */
    }

cleanup:
    if (fl >= 0)
        (void)fcntl(c->fd, F_SETFL, fl); /* 데이터 단계는 원래대로 blocking */
    if (rc == TLS_OK) {
        c->ssl = ssl;
        c->tls_broken = 0;
        log_msg(LOG_DEBUG, "tls: 핸드셰이크 완료 %s %s", SSL_get_version(ssl),
                SSL_get_cipher_name(ssl));
    } else {
        SSL_free(ssl); /* NULL 이면 아무것도 안 한다. fd 는 닫지 않는다. */
    }
    ERR_clear_error();
    return rc;
}

void tls_close(conn_t *c)
{
    if (c->ssl == NULL)
        return;
    if (!c->tls_broken) {
        /*
         * close_notify 를 보낸다 (반환 0 = 보냈고 상대 것은 아직, 1 = 양쪽 완료).
         * 상대의 close_notify 를 기다리지 않는다: 그러려면 한 번 더 읽어야 하는데,
         * 응답 경계는 이미 Content-Length 로 전달됐고 기다리는 동안 워커만 묶인다.
         * 오류 상태(SYSCALL/SSL)에서는 부르면 안 된다 → tls_broken 으로 거른다.
         */
        ERR_clear_error();
        (void)SSL_shutdown(c->ssl);
    }
    ERR_clear_error();
    SSL_free(c->ssl);
    c->ssl = NULL;
}

/* "상대가 그냥 떠났다"로 볼 수 있는 errno (로그를 DEBUG 로 낮춘다) */
static int peer_gone(int err)
{
    return err == 0 || err == EPIPE || err == ECONNRESET;
}

int tls_io_fail(conn_t *c, const char *what, int ret)
{
    int saved = errno;
    int e = SSL_get_error(c->ssl, ret);
    unsigned long q = ERR_peek_error();

    switch (e) {
    case SSL_ERROR_ZERO_RETURN: /* 상대가 close_notify 를 보냄: 정상 종료 */
        log_msg(LOG_DEBUG, "tls: %s: 상대가 TLS 종료(close_notify)", what);
        return TLS_IO_EOF;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        /* blocking 소켓 + AUTO_RETRY 에서 WANT_* 는 SO_RCVTIMEO/SO_SNDTIMEO 만료뿐이다 */
        log_msg(e == SSL_ERROR_WANT_WRITE ? LOG_WARN : LOG_DEBUG, "tls: %s: %s (소켓 타임아웃)",
                what, ssl_err_name(e));
        ERR_clear_error();
        if (e == SSL_ERROR_WANT_WRITE) /* SSL_write 는 같은 인자로만 재시도할 수 있다 → 포기 */
            c->tls_broken = 1;
        return TLS_IO_TIMEOUT;
    case SSL_ERROR_SYSCALL:
        c->tls_broken = 1;
        if (q == 0 && peer_gone(saved)) {
            log_msg(LOG_DEBUG, "tls: %s: 상대가 끊음 (%s, errno %d)", what, ssl_err_name(e), saved);
            return TLS_IO_EOF;
        }
        log_msg(LOG_WARN, "tls: %s 실패: %s (errno %d: %s)", what, ssl_err_name(e), saved,
                saved ? strerror(saved) : "-");
        tls_log_queue(LOG_WARN);
        return TLS_IO_ERROR;
    default: /* SSL_ERROR_SSL: 프로토콜 오류 */
        c->tls_broken = 1;
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
        /* OpenSSL 3: close_notify 없이 TCP 가 끊기면 SYSCALL 대신 이 오류로 보고된다 */
        if (ERR_GET_LIB(q) == ERR_LIB_SSL && ERR_GET_REASON(q) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
            log_msg(LOG_DEBUG, "tls: %s: close_notify 없이 끊김", what);
            ERR_clear_error();
            return TLS_IO_EOF;
        }
#endif
        log_msg(LOG_WARN, "tls: %s 실패: %s", what, ssl_err_name(e));
        tls_log_queue(LOG_WARN);
        return TLS_IO_ERROR;
    }
}
