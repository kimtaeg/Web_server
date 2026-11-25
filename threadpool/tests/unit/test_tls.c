/*
 * test_tls.c — SSL_CTX 생성(인증서·키 검증, TLS 1.2 하한)과 tls_accept 의 성공·실패 경로, tls_close.
 *
 * 실패 경로마다 "SSL 객체가 해제되고 c.ssl == NULL" 인지 본다. 실제 누수 여부는 ASan(make asan),
 * leaks/valgrind(make leakcheck / make valgrind)가 이 테스트를 돌리며 확인한다.
 */
#include "test.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include "log.h"
#include "tls.h"
#include "tls_fixture.h"

static tls_fixture_t fx;

static long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void test_ctx_create(void)
{
    SSL_CTX *ctx = tls_ctx_create(fx.cert, fx.key);

    REQUIRE(ctx != NULL);
    CHECK_EQ_INT((int)SSL_CTX_get_min_proto_version(ctx), TLS1_2_VERSION);
    CHECK((SSL_CTX_get_mode(ctx) & SSL_MODE_AUTO_RETRY) != 0);
    tls_ctx_free(ctx);
    tls_ctx_free(NULL); /* NULL 안전 */
}

static void test_ctx_create_errors(void)
{
    /* 각 경우 시작을 거부해야 한다 (NULL) */
    CHECK(tls_ctx_create("/nonexistent/server.crt", fx.key) == NULL);
    CHECK(tls_ctx_create(fx.cert, "/nonexistent/server.key") == NULL);
    CHECK(tls_ctx_create(fx.cert, fx.key2) == NULL); /* 인증서와 짝이 아닌 키 */
    CHECK(tls_ctx_create(fx.junk, fx.key) == NULL);  /* PEM 형식이 깨진 인증서 */
    CHECK(tls_ctx_create(fx.key, fx.cert) == NULL);  /* 인증서·키 자리를 바꿈 */
    CHECK_EQ_INT((int)ERR_peek_error(), 0);          /* 오류 큐는 로그로 옮기고 비웠다 */
}

static void test_handshake_ok(void)
{
    SSL_CTX *sctx = tls_ctx_create(fx.cert, fx.key);
    SSL_CTX *cctx = fx_client_ctx();
    conn_t srv;
    fx_client_t cl;
    char buf[8];
    int fl;

    REQUIRE(sctx != NULL && cctx != NULL);
    CHECK_EQ_INT(fx_handshake(sctx, cctx, 0, &srv, &cl), TLS_OK);
    CHECK(cl.ok);
    REQUIRE(srv.ssl != NULL);
    CHECK(SSL_version(srv.ssl) >= TLS1_2_VERSION);
    fl = fcntl(srv.fd, F_GETFL);
    CHECK((fl & O_NONBLOCK) == 0); /* 핸드셰이크 후 blocking 으로 복구 */

    /* tls_close: close_notify 를 보내고 SSL 해제 → 클라이언트는 정상 종료(ZERO_RETURN)를 본다 */
    tls_close(&srv);
    CHECK(srv.ssl == NULL);
    CHECK_EQ_INT(SSL_read(cl.ssl, buf, sizeof buf), 0);
    CHECK_EQ_INT(SSL_get_error(cl.ssl, 0), SSL_ERROR_ZERO_RETURN);
    tls_close(&srv); /* 두 번 불러도 안전 */
    close(srv.fd);
    fx_client_free(&cl);
    SSL_CTX_free(cctx);
    tls_ctx_free(sctx);
}

static void test_handshake_old_version_rejected(void)
{
    SSL_CTX *sctx = tls_ctx_create(fx.cert, fx.key);
    SSL_CTX *cctx = fx_client_ctx();
    conn_t srv;
    fx_client_t cl;

    REQUIRE(sctx != NULL && cctx != NULL);
    /* 클라이언트가 TLS 1.1 까지만 → 서버 하한(1.2)에 걸려 실패 */
    CHECK_EQ_INT(fx_handshake(sctx, cctx, TLS1_1_VERSION, &srv, &cl), TLS_FAIL);
    CHECK(!cl.ok);
    CHECK(srv.ssl == NULL); /* 실패 시 SSL 객체를 해제했다 */
    close(srv.fd);
    fx_client_free(&cl);
    SSL_CTX_free(cctx);
    tls_ctx_free(sctx);
}

static void test_plain_http_detected(void)
{
    SSL_CTX *sctx = tls_ctx_create(fx.cert, fx.key);
    conn_t srv;
    int sv[2];
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";

    REQUIRE(sctx != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    REQUIRE(write(sv[1], req, strlen(req)) == (ssize_t)strlen(req));
    conn_init_plain(&srv, sv[0]);
    CHECK_EQ_INT(tls_accept(sctx, &srv, 2000, NULL), TLS_PLAIN_HTTP);
    CHECK(srv.ssl == NULL);
    close(sv[0]);
    close(sv[1]);
    tls_ctx_free(sctx);
}

static void test_garbage_and_eof(void)
{
    SSL_CTX *sctx = tls_ctx_create(fx.cert, fx.key);
    conn_t srv;
    int sv[2];
    long t0;

    REQUIRE(sctx != NULL);
    /* TLS 레코드가 아닌 바이트 */
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    REQUIRE(write(sv[1], "\x01\x02\x03\x04\x05\x06\x07\x08", 8) == 8);
    conn_init_plain(&srv, sv[0]);
    CHECK_EQ_INT(tls_accept(sctx, &srv, 2000, NULL), TLS_FAIL);
    CHECK(srv.ssl == NULL);
    close(sv[0]);
    close(sv[1]);

    /* 아무것도 안 보내고 끊음 → 마감을 기다리지 않고 바로 실패 */
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    conn_init_plain(&srv, sv[0]);
    t0 = ms_now();
    CHECK_EQ_INT(tls_accept(sctx, &srv, 2000, NULL), TLS_FAIL);
    CHECK(ms_now() - t0 < 1000);
    close(sv[0]);
    tls_ctx_free(sctx);
}

static void test_handshake_timeout_and_stop(void)
{
    SSL_CTX *sctx = tls_ctx_create(fx.cert, fx.key);
    volatile sig_atomic_t stop = 1;
    conn_t srv;
    int sv[2];
    long t0, el;

    REQUIRE(sctx != NULL);
    /* 연결만 하고 ClientHello 를 보내지 않는 클라이언트 → 마감(300ms)에 TLS_TIMEOUT */
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    conn_init_plain(&srv, sv[0]);
    t0 = ms_now();
    CHECK_EQ_INT(tls_accept(sctx, &srv, 300, NULL), TLS_TIMEOUT);
    el = ms_now() - t0;
    CHECK(el >= 250 && el < 1500);
    CHECK(srv.ssl == NULL);

    /* 서버 종료 중이면 마감 전이라도 곧바로 포기 */
    t0 = ms_now();
    CHECK_EQ_INT(tls_accept(sctx, &srv, 5000, &stop), TLS_TIMEOUT);
    CHECK(ms_now() - t0 < 1000);
    close(sv[0]);
    close(sv[1]);
    tls_ctx_free(sctx);
}

int main(void)
{
    FILE *devnull = fopen("/dev/null", "w");

    signal(SIGPIPE, SIG_IGN); /* 서버(main)와 같은 전제 */
    log_init(LOG_ERROR, devnull); /* 실패 경로 로그는 버린다 */
    if (fx_setup(&fx) != 0) {
        fprintf(stderr, "인증서 픽스처 생성 실패\n");
        return 1;
    }
    RUN_TEST(test_ctx_create);
    RUN_TEST(test_ctx_create_errors);
    RUN_TEST(test_handshake_ok);
    RUN_TEST(test_handshake_old_version_rejected);
    RUN_TEST(test_plain_http_detected);
    RUN_TEST(test_garbage_and_eof);
    RUN_TEST(test_handshake_timeout_and_stop);
    fx_teardown(&fx);
    log_init(LOG_INFO, NULL);
    if (devnull != NULL)
        fclose(devnull);
    TEST_END();
}
