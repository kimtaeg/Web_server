/*
 * test_conn.c — keep-alive 루프: 파이프라이닝, 유휴 타임아웃, 파싱 오류, 최대 요청 수, 종료 플래그.
 * socketpair 한쪽에서 conn_handle 을 스레드로 돌리고, 다른 쪽을 클라이언트로 쓴다.
 */
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "conn.h"
#include "http_response.h"
#include "log.h"
#include "netio.h"

/* 응답 본문 = 요청 경로. */
static int echo_handler(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci, int ka,
                        handler_result_t *res)
{
    buf_t b;
    int rc = 0;

    (void)ctx;
    (void)ci;
    buf_init(&b, 0, 4096);
    http_response_head(&b, 200, "text/plain", req->path_len, ka, NULL);
    buf_append(&b, req->path, req->path_len);
    if (send_all(fd, b.data, b.len) < 0)
        rc = -1;
    res->status = 200;
    res->bytes = b.len;
    buf_free(&b);
    return rc;
}

typedef struct {
    int fd;
    conn_config_t cfg;
} conn_arg_t;

static void *conn_main(void *p)
{
    conn_arg_t *a = p;

    conn_handle(a->fd, &a->cfg);
    return NULL;
}

static long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void default_cfg(conn_config_t *cfg)
{
    const http_limits_t lim = HTTP_LIMITS_DEFAULT;

    memset(cfg, 0, sizeof *cfg);
    cfg->limits = lim;
    cfg->idle_timeout_ms = 2000;
    cfg->send_timeout_ms = 2000;
    cfg->max_requests = 100;
    cfg->handler = echo_handler;
}

/* conn 을 띄우고, req 를 (step 바이트씩) 보낸 뒤, 서버가 닫을 때까지 받은 전부를 out 에 담는다. */
static long run_session(const conn_config_t *cfg, const char *req, size_t step, buf_t *out)
{
    int sv[2];
    pthread_t th;
    conn_arg_t a;
    char tmp[4096];
    ssize_t n;
    size_t off = 0, len = strlen(req);
    long t0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;
    a.fd = sv[0];
    a.cfg = *cfg;
    t0 = ms_now();
    pthread_create(&th, NULL, conn_main, &a);
    while (off < len) {
        size_t k = step == 0 || step > len - off ? len - off : step;

        send_all(sv[1], req + off, k);
        off += k;
    }
    buf_reset(out);
    while ((n = recv_some(sv[1], tmp, sizeof tmp, 5000)) > 0)
        buf_append(out, tmp, (size_t)n);
    pthread_join(th, NULL); /* conn_handle 이 끝났다 = 연결을 닫았다 */
    close(sv[1]);
    return ms_now() - t0;
}

static int count(const char *hay, const char *needle)
{
    int c = 0;
    const char *p = hay;

    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += strlen(needle);
    }
    return c;
}

static void test_pipelined_in_order(void)
{
    conn_config_t cfg;
    buf_t out;
    const char *one, *two;

    default_cfg(&cfg);
    buf_init(&out, 0, 1 << 20);
    run_session(&cfg,
                "GET /one HTTP/1.1\r\nHost: a\r\n\r\n"
                "GET /two HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n",
                0, &out);
    CHECK_EQ_INT(count(out.data, "HTTP/1.1 200 OK"), 2);
    one = strstr(out.data, "\r\n\r\n/one");
    two = strstr(out.data, "\r\n\r\n/two");
    CHECK(one != NULL && two != NULL && one < two); /* 요청 순서대로 응답 */
    CHECK_EQ_INT(count(out.data, "Connection: keep-alive"), 1);
    CHECK_EQ_INT(count(out.data, "Connection: close"), 1);
    buf_free(&out);
}

static void test_byte_by_byte(void)
{
    conn_config_t cfg;
    buf_t out;

    default_cfg(&cfg);
    buf_init(&out, 0, 1 << 20);
    run_session(&cfg, "GET /slow HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 1, &out);
    CHECK_EQ_INT(count(out.data, "HTTP/1.1 200 OK"), 1);
    CHECK(strstr(out.data, "\r\n\r\n/slow") != NULL);
    buf_free(&out);
}

static void test_http10_closes(void)
{
    conn_config_t cfg;
    buf_t out;

    default_cfg(&cfg);
    buf_init(&out, 0, 1 << 20);
    /* 1.0 은 keep-alive 가 아니므로 첫 응답 후 닫혀야 한다 (두 번째 요청은 무시) */
    run_session(&cfg, "GET /a HTTP/1.0\r\n\r\nGET /b HTTP/1.0\r\n\r\n", 0, &out);
    CHECK_EQ_INT(count(out.data, "HTTP/1.1 200 OK"), 1);
    CHECK(strstr(out.data, "Connection: close") != NULL);
    buf_free(&out);
}

static void test_parse_error_closes(void)
{
    conn_config_t cfg;
    buf_t out;

    default_cfg(&cfg);
    buf_init(&out, 0, 1 << 20);
    /* 오류 뒤의 정상 요청은 처리되면 안 된다: 요청 경계를 잃었기 때문 */
    run_session(&cfg, "BREW /pot HTTP/1.1\r\nHost: a\r\n\r\nGET /x HTTP/1.1\r\nHost: a\r\n\r\n", 0,
                &out);
    CHECK(strncmp(out.data, "HTTP/1.1 501 Not Implemented\r\n", 30) == 0);
    CHECK(strstr(out.data, "Connection: close") != NULL);
    CHECK_EQ_INT(count(out.data, "HTTP/1.1 "), 1);

    run_session(&cfg, "GET / HTTP/1.1\r\n\r\n", 0, &out); /* Host 없음 */
    CHECK(strncmp(out.data, "HTTP/1.1 400 ", 13) == 0);
    buf_free(&out);
}

static void test_idle_timeout(void)
{
    conn_config_t cfg;
    buf_t out;
    long ms;

    default_cfg(&cfg);
    cfg.idle_timeout_ms = 300;
    buf_init(&out, 0, 1 << 20);
    ms = run_session(&cfg, "", 0, &out); /* 아무것도 안 보냄 */
    CHECK_EQ_SIZE(out.len, 0);
    CHECK(ms >= 250);
    CHECK(ms < 2000);

    /* 요청 일부만 보내고 멈춘 경우(slowloris)도 같은 타임아웃으로 끊긴다 */
    ms = run_session(&cfg, "GET / HTTP/1.1\r\nHo", 0, &out);
    CHECK_EQ_SIZE(out.len, 0);
    CHECK(ms < 2000);
    buf_free(&out);
}

static void test_keep_alive_survives_between_requests(void)
{
    conn_config_t cfg;
    buf_t out;

    default_cfg(&cfg);
    cfg.idle_timeout_ms = 300;
    buf_init(&out, 0, 1 << 20);
    /* keep-alive 요청 하나 → 응답 후 연결 유지 → 유휴 타임아웃으로 닫힘 */
    run_session(&cfg, "GET /k HTTP/1.1\r\nHost: a\r\n\r\n", 0, &out);
    CHECK_EQ_INT(count(out.data, "HTTP/1.1 200 OK"), 1);
    CHECK(strstr(out.data, "Connection: keep-alive") != NULL);
    buf_free(&out);
}

static void test_max_requests(void)
{
    conn_config_t cfg;
    buf_t out;
    const char *second;

    default_cfg(&cfg);
    cfg.max_requests = 2;
    buf_init(&out, 0, 1 << 20);
    run_session(&cfg,
                "GET /1 HTTP/1.1\r\nHost: a\r\n\r\n"
                "GET /2 HTTP/1.1\r\nHost: a\r\n\r\n"
                "GET /3 HTTP/1.1\r\nHost: a\r\n\r\n",
                0, &out);
    CHECK_EQ_INT(count(out.data, "HTTP/1.1 200 OK"), 2);
    second = strstr(out.data + 1, "HTTP/1.1 200 OK");
    REQUIRE(second != NULL);
    CHECK(strstr(second, "Connection: close") != NULL); /* 마지막 응답이 닫힘을 알림 */
    CHECK(strstr(out.data, "/3") == NULL);
    buf_free(&out);
}

static void test_stop_flag(void)
{
    static volatile sig_atomic_t stop = 1;
    conn_config_t cfg;
    buf_t out;
    long ms;

    default_cfg(&cfg);
    cfg.idle_timeout_ms = 10000;
    cfg.stop = &stop;
    buf_init(&out, 0, 1 << 20);
    ms = run_session(&cfg, "", 0, &out);
    CHECK(ms < 1000); /* 10초 유휴 타임아웃을 기다리지 않는다 */
    buf_free(&out);
}

/*
 * 클라이언트가 요청을 보내다 말고 끊는 경우.
 * 유휴 타임아웃을 10초로 길게 두고, conn_handle 이 그보다 훨씬 빨리 반환하는지 본다.
 * (빨리 반환 = recv 가 EOF 를 감지해 워커가 풀려났다는 뜻. 타임아웃까지 붙잡혀 있지 않음)
 * fd 가 실제로 닫혔는지는 같은 번호를 다시 fcntl 해서 EBADF 인지로 확인한다.
 */
static void test_client_aborts_mid_request(void)
{
    static const char *partials[] = {
        "GET / HTTP/1.1\r\nHo",                                         /* 헤더 중간 */
        "GET /aaaaaaaaaaaaaaaaaaaaaaaa",                                /* 요청줄 중간 */
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\nab", /* 본문 중간 */
        "",                                                             /* 아무것도 안 보냄 */
    };
    size_t i;

    for (i = 0; i < sizeof partials / sizeof partials[0]; i++) {
        conn_config_t cfg;
        conn_arg_t a;
        pthread_t th;
        int sv[2];
        long t0, ms;

        default_cfg(&cfg);
        cfg.idle_timeout_ms = 10000;
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        a.fd = sv[0];
        a.cfg = cfg;
        t0 = ms_now();
        pthread_create(&th, NULL, conn_main, &a);
        send_all(sv[1], partials[i], strlen(partials[i]));
        close(sv[1]); /* 요청 중간에 끊음 */
        pthread_join(th, NULL);
        ms = ms_now() - t0;
        if (ms >= 1000)
            fprintf(stderr, "    partial #%zu: %ldms\n", i, ms);
        CHECK(ms < 1000);
        CHECK(fcntl(sv[0], F_GETFD) == -1 && errno == EBADF); /* 서버 쪽 fd 회수됨 */
    }
}

/*
 * slowloris: 헤더를 100ms 에 1바이트씩. idle_timeout(2초)은 바이트마다 다시 시작되므로 걸리지 않는다.
 * 헤더 마감(header_timeout)만이 이 연결을 끊을 수 있다.
 *   header_timeout = 600ms → 약 0.6초 뒤 408 + 종료
 *   header_timeout = 0(끔) → 3초 동안 계속 붙잡혀 있다 (= 방어가 없을 때의 문제를 재현)
 */
static long slowloris(int header_timeout_ms, buf_t *got)
{
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\nX-Slow: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
    conn_config_t cfg;
    conn_arg_t a;
    pthread_t th;
    int sv[2];
    long t0, ms = -1;
    size_t i;

    default_cfg(&cfg);
    cfg.idle_timeout_ms = 2000;
    cfg.header_timeout_ms = header_timeout_ms;
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    a.fd = sv[0];
    a.cfg = cfg;
    t0 = ms_now();
    pthread_create(&th, NULL, conn_main, &a);
    buf_reset(got);
    for (i = 0; i < strlen(req) && ms_now() - t0 < 3000; i++) {
        char tmp[1024];
        ssize_t n;
        struct timespec ts = {0, 100 * 1000 * 1000};

        if (send_all(sv[1], req + i, 1) < 0)
            break; /* 서버가 이미 닫음 */
        n = recv_some(sv[1], tmp, sizeof tmp, 0);
        if (n > 0) {
            buf_append(got, tmp, (size_t)n);
            break;
        }
        if (n == 0)
            break;
        nanosleep(&ts, NULL);
    }
    if (got->len > 0 || ms_now() - t0 < 3000) {
        char tmp[1024];
        ssize_t n;

        while ((n = recv_some(sv[1], tmp, sizeof tmp, 1000)) > 0)
            buf_append(got, tmp, (size_t)n);
        ms = ms_now() - t0;
    }
    close(sv[1]); /* 끔(0) 인 경우: 여기서 끊어야 conn 이 끝난다 */
    pthread_join(th, NULL);
    close(sv[0]);
    return ms;
}

static void test_slowloris_header_deadline(void)
{
    buf_t got;
    long ms;

    buf_init(&got, 0, 1 << 16);
    ms = slowloris(600, &got);
    CHECK(got.len > 0 && strncmp(got.data, "HTTP/1.1 408 Request Timeout\r\n", 30) == 0);
    CHECK(strstr(got.data ? got.data : "", "Connection: close") != NULL);
    CHECK(ms >= 500 && ms < 1500); /* idle(2초)이 아니라 헤더 마감(0.6초)에 끊김 */

    /* 방어를 끄면 3초가 지나도 응답이 없다 (서버가 계속 붙잡혀 있음) */
    ms = slowloris(0, &got);
    CHECK_EQ_SIZE(got.len, 0);
    CHECK_EQ_INT(ms, -1);
    buf_free(&got);
}

/* 느리지만 마감 안에 끝나는 정상 요청은 통과해야 한다 */
static void test_slow_but_within_deadline(void)
{
    conn_config_t cfg;
    buf_t out;

    default_cfg(&cfg);
    cfg.header_timeout_ms = 3000;
    buf_init(&out, 0, 1 << 16);
    run_session(&cfg, "GET /ok HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 4, &out);
    CHECK(strncmp(out.data ? out.data : "", "HTTP/1.1 200 OK", 15) == 0);
    buf_free(&out);
}

int main(void)
{
    FILE *devnull = fopen("/dev/null", "w");

    log_init(LOG_WARN, devnull); /* 접근 로그로 테스트 출력이 지저분해지지 않게 */
    RUN_TEST(test_pipelined_in_order);
    RUN_TEST(test_byte_by_byte);
    RUN_TEST(test_http10_closes);
    RUN_TEST(test_parse_error_closes);
    RUN_TEST(test_idle_timeout);
    RUN_TEST(test_keep_alive_survives_between_requests);
    RUN_TEST(test_max_requests);
    RUN_TEST(test_stop_flag);
    RUN_TEST(test_client_aborts_mid_request);
    RUN_TEST(test_slowloris_header_deadline);
    RUN_TEST(test_slow_but_within_deadline);
    log_init(LOG_INFO, NULL);
    if (devnull)
        fclose(devnull);
    TEST_END();
}
