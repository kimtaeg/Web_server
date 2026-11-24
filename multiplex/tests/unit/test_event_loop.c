/*
 * test_event_loop.c — 이벤트 루프 상태 머신을 socketpair 로 직접 돌려 본다 (리슨 소켓 없이).
 *
 * 각 테스트는 ev_loop_add_conn 으로 서버 쪽 소켓을 넣고, 클라이언트 쪽 소켓으로 바이트를 보낸 뒤
 * pump() 로 루프를 조금씩 돌리며 응답을 모은다. 스레드가 없으므로 결과가 결정적이다.
 */
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "event_loop.h"
#include "log.h"
#include "netio.h"
#include "platform.h"
#include "static.h"

static char base[256];
static static_ctx_t sctx;
#define BIG_SIZE (3 * 1024 * 1024)

static long long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static void write_file(const char *rel, const char *content, size_t n)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", base, rel);
    f = fopen(p, "w");
    if (f == NULL || fwrite(content, 1, n, f) != n)
        abort();
    fclose(f);
}

static int setup(void)
{
    char *big;

    snprintf(base, sizeof base, "/tmp/cweb-evtest-XXXXXX");
    if (mkdtemp(base) == NULL)
        return -1;
    write_file("index.html", "<h1>home</h1>", 13);
    write_file("a.txt", "hello", 5);
    big = malloc(BIG_SIZE);
    if (big == NULL)
        return -1;
    memset(big, 'B', BIG_SIZE);
    write_file("big.bin", big, BIG_SIZE);
    free(big);
    return static_init(&sctx, base);
}

static void teardown(void)
{
    char cmd[600];

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0)
        fprintf(stderr, "    정리 실패: %s\n", base);
}

static ev_config_t default_cfg(void)
{
    const http_limits_t lim = HTTP_LIMITS_DEFAULT;
    ev_config_t c;

    memset(&c, 0, sizeof c);
    c.limits = lim;
    c.idle_timeout_ms = 2000;
    c.header_timeout_ms = 2000;
    c.send_timeout_ms = 2000;
    c.max_requests = 100;
    c.max_conns = 1000;
    c.shutdown_grace_ms = 1000;
    c.sctx = &sctx;
    return c;
}

/* 서버 쪽은 루프에, 클라이언트 쪽 fd 를 돌려준다 (논블로킹) */
static int connect_pair(ev_loop_t *l)
{
    int sv[2];
    int fl;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;
    platform_sock_init(sv[0]);
    platform_sock_init(sv[1]);
    if (ev_loop_add_conn(l, sv[0]) != 0) {
        close(sv[1]);
        return -1;
    }
    fl = fcntl(sv[1], F_GETFL);
    fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
    return sv[1];
}

/* 클라이언트 쪽에서 지금 받을 수 있는 것을 out 에 모은다. EOF 면 1. */
static int drain_client(int fd, buf_t *out)
{
    char tmp[65536];

    for (;;) {
        ssize_t n = netio_recv_nb(fd, tmp, sizeof tmp);

        if (n > 0) {
            buf_append(out, tmp, (size_t)n);
            continue;
        }
        return n == 0 || n == -1;
    }
}

/* 응답 count 개가 모이거나 EOF 또는 limit_ms 가 될 때까지 루프를 돌린다. 반환: EOF 여부 */
static int pump_until(ev_loop_t *l, int cfd, buf_t *out, int want_responses, int limit_ms)
{
    long long end = ms_now() + limit_ms;
    int eof = 0;

    while (ms_now() < end) {
        int got = 0;
        ssize_t at = 0;

        ev_loop_run_once(l, 5);
        if (cfd < 0) /* 클라이언트 없이 루프만 돌린다 */
            continue;
        eof = drain_client(cfd, out);
        while ((at = buf_find(out, (size_t)at, "HTTP/1.1 ", 9)) >= 0) {
            got++;
            at++;
        }
        if (eof || (want_responses > 0 && got >= want_responses))
            break;
    }
    return eof;
}

static int count_responses(const buf_t *b)
{
    ssize_t at = 0;
    int got = 0;

    while ((at = buf_find(b, (size_t)at, "HTTP/1.1 ", 9)) >= 0) {
        got++;
        at++;
    }
    return got;
}

static int status_of(const buf_t *b)
{
    if (b->len < 12 || strncmp(b->data, "HTTP/1.1 ", 9) != 0)
        return -1;
    return atoi(b->data + 9);
}

static void send_str(int fd, const char *s)
{
    REQUIRE(netio_send_nb(fd, s, strlen(s)) == (ssize_t)strlen(s));
}

/* ------------------------------------------------------------------ */

static void test_basic_get_keepalive(void)
{
    ev_config_t cfg = default_cfg();
    ev_loop_t *l = ev_loop_create(&cfg, -1);
    buf_t out;
    int c;

    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    REQUIRE(c >= 0);
    send_str(c, "GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ_INT(pump_until(l, c, &out, 1, 1000), 0);
    CHECK_EQ_INT(status_of(&out), 200);
    CHECK(strstr(out.data, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(out.len >= 5 && memcmp(out.data + out.len - 5, "hello", 5) == 0);
    CHECK_EQ_SIZE(ev_loop_nconns(l), 1); /* keep-alive: 연결 유지 */

    /* 같은 연결로 두 번째 요청 */
    buf_reset(&out);
    send_str(c, "GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    pump_until(l, c, &out, 1, 1000);
    CHECK_EQ_INT(status_of(&out), 200);
    CHECK(strstr(out.data, "<h1>home</h1>") != NULL);

    close(c);
    pump_until(l, -1, &out, 0, 50); /* 루프가 EOF 를 보고 닫는다 */
    CHECK_EQ_SIZE(ev_loop_nconns(l), 0);
    buf_free(&out);
    ev_loop_destroy(l);
}

/* 요청을 1바이트씩 나눠 보내도 같은 응답 (부분 읽기 → 상태 보존) */
static void test_byte_by_byte(void)
{
    const char *req = "GET /a.txt HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n";
    ev_config_t cfg = default_cfg();
    ev_loop_t *l = ev_loop_create(&cfg, -1);
    buf_t out;
    size_t i;
    int c, eof;

    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    for (i = 0; req[i] != '\0'; i++) {
        REQUIRE(netio_send_nb(c, req + i, 1) == 1);
        ev_loop_run_once(l, 1);
        CHECK(i + 1 == strlen(req) || out.len == 0); /* 다 받기 전에는 응답이 없다 */
        drain_client(c, &out);
    }
    eof = pump_until(l, c, &out, 0, 1000);
    CHECK_EQ_INT(eof, 1); /* Connection: close → 응답 후 닫힘 */
    CHECK_EQ_INT(status_of(&out), 200);
    CHECK(strstr(out.data, "\r\nConnection: close\r\n") != NULL);
    close(c);
    buf_free(&out);
    ev_loop_destroy(l);
}

/* 파이프라이닝: 한 번에 온 요청 3개를 순서대로, 새 이벤트 없이 모두 처리 */
static void test_pipelining(void)
{
    ev_config_t cfg = default_cfg();
    ev_loop_t *l = ev_loop_create(&cfg, -1);
    buf_t out;
    const char *p1, *p2, *p3;
    int c;

    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    send_str(c, "GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n"
                "GET /nope HTTP/1.1\r\nHost: a\r\n\r\n"
                "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    CHECK_EQ_INT(pump_until(l, c, &out, 0, 1000), 1);
    CHECK_EQ_INT(count_responses(&out), 3);
    p1 = strstr(out.data, "200 OK");
    p2 = strstr(out.data, "404 Not Found");
    p3 = p2 ? strstr(p2, "200 OK") : NULL;
    CHECK(p1 != NULL && p2 != NULL && p3 != NULL && p1 < p2 && p2 < p3); /* 순서 보장 */
    close(c);
    buf_free(&out);
    ev_loop_destroy(l);
}

/* 파싱 오류 → 상태 코드 응답 후 연결 종료 */
static void test_parse_errors_close(void)
{
    static const struct {
        const char *raw;
        int status;
    } cases[] = {
        {"GARBAGE\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n\r\n", 400}, /* Host 없음 */
        {"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505},
        {"BREW / HTTP/1.1\r\nHost: a\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nHost: a\r\n\r\n", 411},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ev_config_t cfg = default_cfg();
        ev_loop_t *l = ev_loop_create(&cfg, -1);
        buf_t out;
        int c;

        REQUIRE(l != NULL);
        buf_init(&out, 0, 1 << 20);
        c = connect_pair(l);
        send_str(c, cases[i].raw);
        CHECK_EQ_INT(pump_until(l, c, &out, 0, 1000), 1); /* 닫힘 */
        CHECK_EQ_INT(status_of(&out), cases[i].status);
        CHECK(strstr(out.data, "\r\nConnection: close\r\n") != NULL);
        /* 서버는 FIN 을 보낸 뒤 LINGER(남은 입력 비우기) 상태다. 클라이언트가 닫으면 정리된다 */
        CHECK_EQ_SIZE(ev_loop_nconns(l), 1);
        close(c);
        pump_until(l, -1, &out, 0, 50);
        CHECK_EQ_SIZE(ev_loop_nconns(l), 0);
        buf_free(&out);
        ev_loop_destroy(l);
    }
}

/* 유휴 타임아웃: 아무것도 안 보내면 응답 없이 닫는다 */
static void test_idle_timeout(void)
{
    ev_config_t cfg = default_cfg();
    ev_loop_t *l;
    buf_t out;
    long long t0;
    int c, eof;

    cfg.idle_timeout_ms = 100;
    l = ev_loop_create(&cfg, -1);
    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    t0 = ms_now();
    eof = pump_until(l, c, &out, 0, 2000);
    CHECK_EQ_INT(eof, 1);
    CHECK(ms_now() - t0 >= 90);
    CHECK(ms_now() - t0 < 1000);
    CHECK_EQ_SIZE(out.len, 0);
    CHECK_EQ_SIZE(ev_loop_nconns(l), 0);
    close(c);
    buf_free(&out);
    ev_loop_destroy(l);
}

/* slowloris: 유휴 타임아웃보다 짧은 간격으로 조금씩 보내도 헤더 마감에서 408 */
static void test_header_deadline_408(void)
{
    ev_config_t cfg = default_cfg();
    ev_loop_t *l;
    buf_t out;
    long long t0;
    int c, eof = 0;

    cfg.idle_timeout_ms = 1000;
    cfg.header_timeout_ms = 200;
    l = ev_loop_create(&cfg, -1);
    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    send_str(c, "GET / HTTP/1.1\r\n");
    t0 = ms_now();
    while (!eof && ms_now() - t0 < 2000) {
        (void)netio_send_nb(c, "X-a: b\r\n", 8); /* 50ms 마다 한 줄: idle(1s)은 계속 연장됨 */
        eof = pump_until(l, c, &out, 1, 50);
    }
    if (!eof)
        eof = pump_until(l, c, &out, 0, 500);
    CHECK_EQ_INT(eof, 1);
    CHECK_EQ_INT(status_of(&out), 408);
    CHECK(ms_now() - t0 < 1000); /* idle 이 아니라 헤더 마감(200ms)에 걸렸다 */
    close(c);
    buf_free(&out);
    ev_loop_destroy(l);
}

/* 연결당 최대 요청 수: 마지막 응답에 Connection: close 후 닫힘 */
static void test_max_requests(void)
{
    ev_config_t cfg = default_cfg();
    ev_loop_t *l;
    buf_t out;
    int c;

    cfg.max_requests = 2;
    l = ev_loop_create(&cfg, -1);
    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    send_str(c, "GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\nGET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ_INT(pump_until(l, c, &out, 0, 1000), 1);
    CHECK_EQ_INT(count_responses(&out), 2);
    {
        const char *second = strstr(out.data + 1, "HTTP/1.1 ");

        REQUIRE(second != NULL);
        CHECK(strstr(out.data, "Connection: keep-alive") < second);
        CHECK(strstr(second, "\r\nConnection: close\r\n") != NULL);
    }
    close(c);
    buf_free(&out);
    ev_loop_destroy(l);
}

/*
 * 큰 파일 + 읽지 않는 클라이언트: 송신 버퍼가 차면 루프는 그 연결을 WRITE 상태로 두고(EAGAIN)
 * 다른 연결을 계속 처리한다. 이후 클라이언트가 읽기 시작하면 이어서 끝까지 보낸다.
 */
static void test_backpressure_does_not_block_others(void)
{
    ev_config_t cfg = default_cfg();
    ev_loop_t *l = ev_loop_create(&cfg, -1);
    buf_t big, small;
    long long t0;
    int slow, fast;
    size_t body;

    REQUIRE(l != NULL);
    buf_init(&big, 0, BIG_SIZE + 4096);
    buf_init(&small, 0, 1 << 20);
    slow = connect_pair(l);
    fast = connect_pair(l);
    send_str(slow, "GET /big.bin HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    for (t0 = ms_now(); ms_now() - t0 < 100;) /* slow 는 읽지 않는다 → 송신 버퍼 가득 */
        ev_loop_run_once(l, 5);

    send_str(fast, "GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n");
    pump_until(l, fast, &small, 1, 1000);
    CHECK_EQ_INT(status_of(&small), 200); /* 큰 전송이 막혀 있어도 다른 연결은 바로 응답 */
    CHECK_EQ_SIZE(ev_loop_nconns(l), 2);

    CHECK_EQ_INT(pump_until(l, slow, &big, 0, 5000), 1); /* 이제 읽으면 끝까지 온다 */
    {
        const char *hdr_end = strstr(big.data, "\r\n\r\n");

        REQUIRE(hdr_end != NULL);
        body = big.len - (size_t)(hdr_end + 4 - big.data);
        CHECK_EQ_SIZE(body, BIG_SIZE);
    }
    close(slow);
    close(fast);
    buf_free(&big);
    buf_free(&small);
    ev_loop_destroy(l);
}

/* 스레드 하나로 유휴 연결 수백 개 + 활성 요청: 워커 수에 묶이지 않는다 (이 구현의 존재 이유) */
static void test_many_idle_connections(void)
{
    enum { IDLE = 300 };
    ev_config_t cfg = default_cfg();
    ev_loop_t *l = ev_loop_create(&cfg, -1);
    int idle[IDLE];
    buf_t out;
    int i, c, opened = 0;

    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    for (i = 0; i < IDLE; i++) {
        idle[i] = connect_pair(l);
        if (idle[i] < 0)
            break; /* fd 한도 (ulimit -n) */
        opened++;
    }
    CHECK(opened >= 100);
    CHECK_EQ_SIZE(ev_loop_nconns(l), (size_t)opened);
    c = connect_pair(l);
    if (c >= 0) {
        send_str(c, "GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n");
        pump_until(l, c, &out, 1, 1000);
        CHECK_EQ_INT(status_of(&out), 200);
        close(c);
    }
    for (i = 0; i < opened; i++)
        close(idle[i]);
    pump_until(l, -1, &out, 0, 100);
    CHECK_EQ_SIZE(ev_loop_nconns(l), 0);
    buf_free(&out);
    ev_loop_destroy(l);
}

/* graceful: stop 이 서면 대기 연결은 바로 닫고, ev_loop_run 이 반환한다 */
static void test_stop_closes_idle(void)
{
    static volatile sig_atomic_t stop = 0;
    ev_config_t cfg = default_cfg();
    ev_loop_t *l;
    buf_t out;
    long long t0;
    int c;

    cfg.stop = &stop;
    l = ev_loop_create(&cfg, -1);
    REQUIRE(l != NULL);
    buf_init(&out, 0, 1 << 20);
    c = connect_pair(l);
    stop = 1;
    t0 = ms_now();
    CHECK_EQ_INT(ev_loop_run(l), 0);
    CHECK(ms_now() - t0 < 1000);
    CHECK_EQ_SIZE(ev_loop_nconns(l), 0);
    CHECK_EQ_INT(drain_client(c, &out), 1); /* 클라이언트는 EOF 를 본다 */
    close(c);
    buf_free(&out);
    ev_loop_destroy(l);
}

int main(void)
{
    log_init(LOG_ERROR, NULL); /* 접근 로그(INFO)는 테스트 출력에서 숨긴다 */
    if (setup() != 0) {
        fprintf(stderr, "임시 디렉터리 준비 실패\n");
        return 1;
    }
    RUN_TEST(test_basic_get_keepalive);
    RUN_TEST(test_byte_by_byte);
    RUN_TEST(test_pipelining);
    RUN_TEST(test_parse_errors_close);
    RUN_TEST(test_idle_timeout);
    RUN_TEST(test_header_deadline_408);
    RUN_TEST(test_max_requests);
    RUN_TEST(test_backpressure_does_not_block_others);
    RUN_TEST(test_many_idle_connections);
    RUN_TEST(test_stop_closes_idle);
    teardown();
    TEST_END();
}
