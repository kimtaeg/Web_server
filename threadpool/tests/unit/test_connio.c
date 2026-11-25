/*
 * test_connio.c — conn_read/conn_write: 평문은 기존 netio 와 같은 동작, TLS 는 SSL_read/SSL_write.
 *   부분 쓰기(큰 응답), SSL_pending 회귀, 정상/비정상 종료, 끊긴 상대에게 쓰기,
 *   여러 연결의 SSL 객체가 서로 독립적인지(동시 연결 16개).
 */
#include "test.h"

#include <poll.h>
#include <signal.h>
#include <time.h>

#include "connio.h"
#include "log.h"
#include "netio.h"
#include "tls.h"
#include "tls_fixture.h"

static tls_fixture_t fx;
static SSL_CTX *g_sctx;
static SSL_CTX *g_cctx;

static long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ssl == NULL 이면 send_all / recv_some 과 똑같이 동작해야 한다 (기존 HTTP 경로) */
static void test_plain_passthrough(void)
{
    conn_t c;
    int sv[2];
    char buf[16];

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    conn_init_plain(&c, sv[0]);
    CHECK(c.ssl == NULL);
    CHECK_EQ_INT((int)conn_write(&c, "hello", 5), 5);
    CHECK_EQ_INT((int)recv_some(sv[1], buf, sizeof buf, 1000), 5);
    CHECK_EQ_MEM(buf, "hello", 5);
    CHECK_EQ_INT((int)conn_read(&c, buf, sizeof buf, 50), NETIO_TIMEOUT);
    CHECK_EQ_INT((int)write(sv[1], "abc", 3), 3);
    CHECK_EQ_INT((int)conn_read(&c, buf, sizeof buf, 1000), 3);
    CHECK_EQ_MEM(buf, "abc", 3);
    close(sv[1]);
    CHECK_EQ_INT((int)conn_read(&c, buf, sizeof buf, 1000), 0); /* EOF */
    close(sv[0]);
}

static void test_tls_roundtrip(void)
{
    conn_t srv;
    fx_client_t cl;
    char buf[16];

    REQUIRE(fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) == TLS_OK);
    CHECK_EQ_INT((int)conn_write(&srv, "ping", 4), 4);
    CHECK_EQ_INT(SSL_read(cl.ssl, buf, sizeof buf), 4);
    CHECK_EQ_MEM(buf, "ping", 4);
    CHECK_EQ_INT(SSL_write(cl.ssl, "pong!", 5), 5);
    CHECK_EQ_INT((int)conn_read(&srv, buf, sizeof buf, 1000), 5);
    CHECK_EQ_MEM(buf, "pong!", 5);
    /* 아무것도 안 옴 → 타임아웃. 연결은 멀쩡하다 */
    CHECK_EQ_INT((int)conn_read(&srv, buf, sizeof buf, 50), NETIO_TIMEOUT);
    CHECK_EQ_INT(srv.tls_broken, 0);
    CHECK_EQ_INT((int)conn_write(&srv, "x", 1), 1);
    CHECK_EQ_INT(SSL_read(cl.ssl, buf, sizeof buf), 1);
    tls_close(&srv);
    close(srv.fd);
    fx_client_free(&cl);
}

typedef struct {
    SSL *ssl;
    size_t want;
    size_t got;
    int bad; /* 내용 불일치 */
} reader_t;

static void *reader_main(void *p)
{
    reader_t *r = p;
    static char buf[65536];

    while (r->got < r->want) {
        int n = SSL_read(r->ssl, buf, sizeof buf);
        int i;

        if (n <= 0)
            break;
        for (i = 0; i < n; i++)
            if ((unsigned char)buf[i] != (unsigned char)((r->got + (size_t)i) * 7 % 251))
                r->bad = 1;
        r->got += (size_t)n;
    }
    return NULL;
}

/* 소켓 버퍼(수십 KB)와 TLS 레코드(16KB)보다 훨씬 큰 응답: 부분 쓰기·여러 레코드가 이어져야 한다 */
static void test_tls_large_write(void)
{
    enum { N = 3 * 1024 * 1024 + 123 };
    static char big[N];
    conn_t srv;
    fx_client_t cl;
    reader_t r;
    pthread_t th;
    size_t i;

    for (i = 0; i < N; i++)
        big[i] = (char)(i * 7 % 251);
    REQUIRE(fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) == TLS_OK);
    memset(&r, 0, sizeof r);
    r.ssl = cl.ssl;
    r.want = N;
    REQUIRE(pthread_create(&th, NULL, reader_main, &r) == 0);
    CHECK_EQ_INT((int)conn_write(&srv, big, N), N);
    pthread_join(th, NULL);
    CHECK_EQ_SIZE(r.got, (size_t)N);
    CHECK_EQ_INT(r.bad, 0);
    tls_close(&srv);
    close(srv.fd);
    fx_client_free(&cl);
}

/*
 * 회귀 테스트: 레코드 하나에 8바이트("AAAABBBB")가 왔는데 4바이트만 읽으면, 나머지 4바이트는
 * OpenSSL 내부 버퍼에 있고 커널 소켓 버퍼는 비어 있다. poll 부터 하면 타임아웃까지 멈춘다.
 * conn_read 는 SSL_pending 을 먼저 보고 바로 돌려줘야 한다.
 */
static void test_ssl_pending_not_lost(void)
{
    conn_t srv;
    fx_client_t cl;
    struct pollfd pfd;
    char buf[8];
    long t0;

    REQUIRE(fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) == TLS_OK);
    CHECK_EQ_INT(SSL_write(cl.ssl, "AAAABBBB", 8), 8);
    CHECK_EQ_INT((int)conn_read(&srv, buf, 4, 1000), 4);
    CHECK_EQ_MEM(buf, "AAAA", 4);
    CHECK_EQ_INT(SSL_pending(srv.ssl), 4);

    pfd.fd = srv.fd; /* 커널 버퍼는 비었다: poll 만 믿으면 놓친다 */
    pfd.events = POLLIN;
    pfd.revents = 0;
    CHECK_EQ_INT(poll(&pfd, 1, 0), 0);

    t0 = ms_now();
    CHECK_EQ_INT((int)conn_read(&srv, buf, 4, 2000), 4);
    CHECK(ms_now() - t0 < 500);
    CHECK_EQ_MEM(buf, "BBBB", 4);
    tls_close(&srv);
    close(srv.fd);
    fx_client_free(&cl);
}

static void test_tls_close_notify_eof(void)
{
    conn_t srv;
    fx_client_t cl;
    char buf[8];

    REQUIRE(fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) == TLS_OK);
    (void)SSL_shutdown(cl.ssl); /* 클라이언트가 정상 종료 (close_notify) */
    CHECK_EQ_INT((int)conn_read(&srv, buf, sizeof buf, 1000), 0);
    CHECK_EQ_INT(srv.tls_broken, 0); /* 정상 종료: 우리도 close_notify 를 보내도 된다 */
    tls_close(&srv);
    CHECK(srv.ssl == NULL);
    close(srv.fd);
    fx_client_free(&cl);
}

static void test_tls_abrupt_eof_and_gone_peer(void)
{
    enum { N = 1024 * 1024 };
    static char big[N];
    conn_t srv;
    fx_client_t cl;
    char buf[8];

    /* close_notify 없이 TCP 만 끊김 → EOF 로 다루되, 오류 상태라 SSL_shutdown 은 하지 않는다 */
    REQUIRE(fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) == TLS_OK);
    fx_client_free(&cl);
    CHECK_EQ_INT((int)conn_read(&srv, buf, sizeof buf, 1000), 0);
    CHECK_EQ_INT(srv.tls_broken, 1);
    CHECK_EQ_INT((int)conn_write(&srv, "x", 1), -1); /* 이후 I/O 는 바로 실패 */
    tls_close(&srv);
    close(srv.fd);

    /* 이미 떠난 상대에게 큰 응답 쓰기 → -1 (EPIPE). 프로세스가 죽지 않는다 (SIGPIPE 무시) */
    REQUIRE(fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) == TLS_OK);
    fx_client_free(&cl);
    memset(big, 'z', sizeof big);
    CHECK_EQ_INT((int)conn_write(&srv, big, sizeof big), -1);
    CHECK_EQ_INT(srv.tls_broken, 1);
    tls_close(&srv);
    close(srv.fd);
}

/* ---- 여러 연결: 연결마다 SSL 객체가 따로, 데이터가 섞이지 않는다 ---- */

enum { NCONN = 16, ROUNDS = 20 };

typedef struct {
    int id;
    int ok;
} pair_t;

static void *pair_main(void *p)
{
    pair_t *pr = p;
    conn_t srv;
    fx_client_t cl;
    int k;

    if (fx_handshake(g_sctx, g_cctx, 0, &srv, &cl) != TLS_OK)
        return NULL;
    pr->ok = 1;
    for (k = 0; k < ROUNDS && pr->ok; k++) {
        char msg[64], echo[64], got[64];
        int n = snprintf(msg, sizeof msg, "conn-%d round-%d", pr->id, k);
        ssize_t r;

        /* 클라이언트 → 서버: 서버는 받은 것을 그대로 돌려준다 */
        if (SSL_write(cl.ssl, msg, n) != n)
            pr->ok = 0;
        r = conn_read(&srv, echo, sizeof echo, 2000);
        if (r != n || memcmp(echo, msg, (size_t)n) != 0 || conn_write(&srv, echo, (size_t)r) != r)
            pr->ok = 0;
        if (SSL_read(cl.ssl, got, sizeof got) != n || memcmp(got, msg, (size_t)n) != 0)
            pr->ok = 0;
    }
    tls_close(&srv);
    close(srv.fd);
    fx_client_free(&cl);
    return NULL;
}

static void test_many_connections_independent(void)
{
    pthread_t th[NCONN];
    pair_t pr[NCONN];
    int i, all_ok = 1;

    for (i = 0; i < NCONN; i++) {
        memset(&pr[i], 0, sizeof pr[i]);
        pr[i].id = i;
        REQUIRE(pthread_create(&th[i], NULL, pair_main, &pr[i]) == 0);
    }
    for (i = 0; i < NCONN; i++)
        pthread_join(th[i], NULL);
    for (i = 0; i < NCONN; i++)
        all_ok &= pr[i].ok;
    /* 각 연결이 자기 메시지만 정확히 돌려받았다 = SSL 상태(키, 시퀀스 번호, 버퍼)가 섞이지 않았다 */
    CHECK(all_ok);
}

int main(void)
{
    FILE *devnull = fopen("/dev/null", "w");

    signal(SIGPIPE, SIG_IGN); /* 서버(main)와 같은 전제 */
    log_init(LOG_ERROR, devnull);
    if (fx_setup(&fx) != 0) {
        fprintf(stderr, "인증서 픽스처 생성 실패\n");
        return 1;
    }
    g_sctx = tls_ctx_create(fx.cert, fx.key);
    g_cctx = fx_client_ctx();
    if (g_sctx == NULL || g_cctx == NULL) {
        fprintf(stderr, "SSL_CTX 생성 실패\n");
        return 1;
    }
    RUN_TEST(test_plain_passthrough);
    RUN_TEST(test_tls_roundtrip);
    RUN_TEST(test_tls_large_write);
    RUN_TEST(test_ssl_pending_not_lost);
    RUN_TEST(test_tls_close_notify_eof);
    RUN_TEST(test_tls_abrupt_eof_and_gone_peer);
    RUN_TEST(test_many_connections_independent);
    SSL_CTX_free(g_cctx);
    tls_ctx_free(g_sctx);
    fx_teardown(&fx);
    log_init(LOG_INFO, NULL);
    if (devnull != NULL)
        fclose(devnull);
    TEST_END();
}
