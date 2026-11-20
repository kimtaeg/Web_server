/*
 * test_upstream.c — 요청 재작성(테이블), 응답 헤더 파싱, 연결, 응답 중계(socketpair).
 */
#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "http_parser.h"
#include "netio.h"
#include "upstream.h"

static int parse_req(const char *raw, http_request_t *req)
{
    static http_parser_t p; /* req 포인터는 raw 를 가리키므로 raw 는 호출자가 유지 */
    size_t used;

    http_parser_init(&p, NULL);
    return http_parse(&p, raw, strlen(raw), req, &used) == PARSE_OK ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 요청 재작성                                                          */
/* ------------------------------------------------------------------ */

static void test_hop_by_hop(void)
{
    static const char *hop[] = {"Connection", "keep-alive", "TE", "Trailer", "Transfer-Encoding",
                                "Upgrade", "Proxy-Authorization", "proxy-connection"};
    static const char *keep[] = {"Host", "Content-Type", "X-Proxy", "Cookie", "Trailers-X"};
    size_t i;

    for (i = 0; i < sizeof hop / sizeof hop[0]; i++)
        CHECK(upstream_is_hop_by_hop(hop[i], strlen(hop[i])));
    for (i = 0; i < sizeof keep / sizeof keep[0]; i++)
        CHECK(!upstream_is_hop_by_hop(keep[i], strlen(keep[i])));
}

static void test_build_request(void)
{
    static const struct {
        const char *name;
        const char *raw;
        const char *must[8];     /* 결과에 있어야 할 문자열 */
        const char *must_not[8]; /* 결과에 없어야 할 문자열 */
    } cases[] = {
        {"기본 + 추가 헤더",
         "GET /api/x?y=1 HTTP/1.1\r\nHost: front.example\r\nUser-Agent: t\r\n\r\n",
         {"GET /api/x?y=1 HTTP/1.1\r\n", "Host: front.example\r\n", "User-Agent: t\r\n",
          "X-Forwarded-For: 10.1.2.3\r\n", "X-Forwarded-Proto: http\r\n", "Via: 1.1 cweb\r\n",
          "Connection: close\r\n\r\n", NULL},
         {"Content-Length", NULL}},
        {"hop-by-hop 제거",
         "GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive, X-Secret\r\nKeep-Alive: timeout=5\r\n"
         "TE: trailers\r\nTrailer: X\r\nUpgrade: websocket\r\nProxy-Authorization: Basic abc\r\n"
         "X-Secret: s\r\nX-Normal: n\r\n\r\n",
         {"X-Normal: n\r\n", "Connection: close\r\n", NULL},
         {"keep-alive", "Keep-Alive", "TE:", "Trailer:", "Upgrade", "Proxy-Authorization",
          "X-Secret", NULL}},
        {"XFF 덧붙이기, Via 체인",
         "GET / HTTP/1.1\r\nHost: h\r\nX-Forwarded-For: 1.1.1.1\r\nX-Forwarded-For: 2.2.2.2\r\n"
         "Via: 1.0 edge\r\nX-Forwarded-Proto: https\r\n\r\n",
         {"X-Forwarded-For: 1.1.1.1, 2.2.2.2, 10.1.2.3\r\n", "Via: 1.0 edge, 1.1 cweb\r\n",
          "X-Forwarded-Proto: http\r\n", NULL},
         {"X-Forwarded-Proto: https", NULL}},
        {"POST 본문과 CL 보존, Expect 제거",
         "POST /f HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\nhello",
         {"POST /f HTTP/1.1\r\n", "Content-Length: 5\r\n", "\r\n\r\nhello", NULL},
         {"Expect", NULL}},
        {"CL 0 POST",
         "POST /f HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n",
         {"Content-Length: 0\r\n", NULL},
         {NULL}},
        {"HTTP/1.0 + Host 없음 → 백엔드 이름으로 Host, 업스트림은 1.1",
         "GET /old HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
         {"GET /old HTTP/1.1\r\n", "Host: backend:9000\r\n", "Connection: close\r\n", NULL},
         {"keep-alive", NULL}},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request_t req;
        buf_t out;
        size_t k;

        REQUIRE(parse_req(cases[i].raw, &req) == 0);
        buf_init(&out, 0, 1 << 16);
        CHECK_EQ_INT(upstream_build_request(&out, &req, req.target, req.target_len, "backend:9000",
                                            "10.1.2.3"),
                     BUF_OK);
        for (k = 0; cases[i].must[k]; k++) {
            if (strstr(out.data, cases[i].must[k]) == NULL)
                fprintf(stderr, "    [%s] 없음: %s\n---\n%s---\n", cases[i].name, cases[i].must[k],
                        out.data);
            CHECK(strstr(out.data, cases[i].must[k]) != NULL);
        }
        for (k = 0; cases[i].must_not[k]; k++) {
            if (strstr(out.data, cases[i].must_not[k]) != NULL)
                fprintf(stderr, "    [%s] 있으면 안 됨: %s\n", cases[i].name, cases[i].must_not[k]);
            CHECK(strstr(out.data, cases[i].must_not[k]) == NULL);
        }
        /* 결과는 우리 파서로 다시 파싱 가능한 올바른 HTTP 요청이어야 한다 */
        {
            http_parser_t p;
            http_request_t again;
            size_t used;

            http_parser_init(&p, NULL);
            CHECK_EQ_INT(http_parse(&p, out.data, out.len, &again, &used), PARSE_OK);
            CHECK_EQ_SIZE(used, out.len);
            CHECK_EQ_INT(again.keep_alive, 0);
        }
        buf_free(&out);
    }
}

/* ------------------------------------------------------------------ */
/* 응답 헤더 파싱                                                       */
/* ------------------------------------------------------------------ */

static void test_parse_response_head(void)
{
    static const struct {
        const char *in;
        int rc;
        int status;
        int has_cl;
        size_t cl;
        int chunked;
    } cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 0, 200, 1, 5, 0},
        {"HTTP/1.0 404 Not Found\r\n\r\n", 0, 404, 0, 0, 0},
        {"HTTP/1.1 204\r\n\r\n", 0, 204, 0, 0, 0},            /* 사유 없음 */
        {"HTTP/1.1 200 OK\nContent-Length: 2\n\nhi", 0, 200, 1, 2, 0}, /* LF 만 */
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 0, 200, 0, 0, 1},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 0, 200, 0, 0, 1},
        {"HTTP/1.1 200 OK\r\nContent-Length: 9\r\nTransfer-Encoding: chunked\r\n\r\n", 0, 200, 0, 0, 1},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n", 0, 200, 1, 3, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 1, 0, 0, 0, 0},   /* 더 필요 */
        {"HTTP/1.1 200 O", 1, 0, 0, 0, 0},
        {"", 1, 0, 0, 0, 0},
        {"HTTP/2 200 OK\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 20 OK\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 2000 OK\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 099 X\r\n\r\n", -1, 0, 0, 0, 0},
        {"garbage\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 200 OK\r\nBad Header: v\r\n\r\n", -1, 0, 0, 0, 0},
        {"HTTP/1.1 200 OK\r\nX: 1\r\n folded\r\n\r\n", -1, 0, 0, 0, 0},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        upstream_resp_t r;
        int rc = upstream_parse_response_head(cases[i].in, strlen(cases[i].in), &r);

        if (rc != cases[i].rc)
            fprintf(stderr, "    case %zu rc=%d\n", i, rc);
        CHECK_EQ_INT(rc, cases[i].rc);
        if (rc == 0) {
            CHECK_EQ_INT(r.status, cases[i].status);
            CHECK_EQ_INT(r.has_cl, cases[i].has_cl);
            CHECK_EQ_SIZE(r.content_length, cases[i].cl);
            CHECK_EQ_INT(r.chunked, cases[i].chunked);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 연결                                                                */
/* ------------------------------------------------------------------ */

static long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void test_connect(void)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int fd;
    int port;
    long t0;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lfd, 4) == 0);
    REQUIRE(getsockname(lfd, (struct sockaddr *)&sa, &sl) == 0);
    port = ntohs(sa.sin_port);

    fd = upstream_connect((struct sockaddr *)&sa, sizeof sa, 1000);
    CHECK(fd >= 0); /* 열린 포트 */
    if (fd >= 0)
        close(fd);

    close(lfd); /* 이제 닫힌 포트: 즉시 거부(RST) */
    sa.sin_port = htons((unsigned short)port);
    t0 = ms_now();
    CHECK_EQ_INT(upstream_connect((struct sockaddr *)&sa, sizeof sa, 1000), UPSTREAM_ECONN);
    CHECK(ms_now() - t0 < 500); /* 타임아웃까지 기다리지 않는다 */

    /*
     * 응답 없는 주소(TEST-NET-1, RFC 5737): 패킷이 버려지면 타임아웃, 라우팅 불가면 즉시 실패.
     * 환경마다 다르므로 "어느 쪽이든 타임아웃 근처 이내에 돌아온다"만 검사한다.
     */
    sa.sin_addr.s_addr = inet_addr("192.0.2.1");
    sa.sin_port = htons(81);
    t0 = ms_now();
    fd = upstream_connect((struct sockaddr *)&sa, sizeof sa, 300);
    CHECK(fd == UPSTREAM_ETIMEOUT || fd == UPSTREAM_ECONN);
    CHECK(ms_now() - t0 < 1500);
    if (fd >= 0)
        close(fd);
}

/* ------------------------------------------------------------------ */
/* 응답 중계: up[0]=업스트림 쪽(테스트가 씀), cl[1]=클라이언트 쪽(테스트가 읽음) */
/* ------------------------------------------------------------------ */

typedef struct {
    int rc;
    upstream_result_t res;
    buf_t client; /* 클라이언트가 받은 것 */
} relay_out_t;

static void relay(const char *req_raw, const char *backend, int close_backend, int timeout_ms,
                  relay_out_t *o)
{
    http_request_t req;
    int up[2], cl[2];
    char tmp[4096];
    ssize_t n;

    parse_req(req_raw, &req);
    socketpair(AF_UNIX, SOCK_STREAM, 0, up);
    socketpair(AF_UNIX, SOCK_STREAM, 0, cl);
    send_all(up[0], backend, strlen(backend));
    if (close_backend)
        shutdown(up[0], SHUT_WR); /* 백엔드 EOF */
    o->rc = upstream_relay(up[1], cl[0], &req, 1, timeout_ms, &o->res);
    close(cl[0]);
    buf_init(&o->client, 0, 1 << 20);
    while ((n = recv_some(cl[1], tmp, sizeof tmp, 500)) > 0)
        buf_append(&o->client, tmp, (size_t)n);
    close(cl[1]);
    close(up[0]);
    close(up[1]);
}

static const char *GET = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";

static void test_relay_content_length_keeps_alive(void)
{
    relay_out_t o;

    /* 백엔드를 닫지 않아도 CL 만큼 받으면 끝 (뒤의 여분은 버림) */
    relay(GET,
          "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\nKeep-Alive: x\r\n"
          "X-App: 1\r\n\r\nhelloEXTRA",
          0, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_OK);
    CHECK_EQ_INT(o.res.status, 200);
    CHECK_EQ_INT(o.res.client_keep_alive, 1);
    CHECK(strstr(o.client.data, "HTTP/1.1 200 OK\r\n") == o.client.data);
    CHECK(strstr(o.client.data, "Connection: keep-alive\r\n") != NULL); /* 우리 쪽 연결 정책 */
    CHECK(strstr(o.client.data, "Connection: close") == NULL);         /* 백엔드 것은 제거 */
    CHECK(strstr(o.client.data, "Keep-Alive") == NULL);
    CHECK(strstr(o.client.data, "X-App: 1\r\n") != NULL);
    CHECK_EQ_STR(strstr(o.client.data, "\r\n\r\n") + 4, "hello");
    CHECK_EQ_SIZE(o.res.bytes, o.client.len);
    buf_free(&o.client);
}

static void test_relay_until_eof_closes(void)
{
    relay_out_t o;

    relay(GET, "HTTP/1.1 200 OK\r\nContent-Type: a/b\r\n\r\nstream-until-eof", 1, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_OK);
    CHECK_EQ_INT(o.res.client_keep_alive, 0);
    CHECK(strstr(o.client.data, "Connection: close\r\n") != NULL);
    CHECK_EQ_STR(strstr(o.client.data, "\r\n\r\n") + 4, "stream-until-eof");
    buf_free(&o.client);
}

static void test_relay_chunked(void)
{
    relay_out_t o;
    const char *body = "5\r\nhello\r\n0\r\n\r\n";
    char backend[256];

    snprintf(backend, sizeof backend,
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 99\r\n\r\n%s", body);
    relay(GET, backend, 1, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_OK);
    CHECK(strstr(o.client.data, "Transfer-Encoding: chunked\r\n") != NULL);
    CHECK(strstr(o.client.data, "Content-Length") == NULL); /* TE 가 있으면 CL 은 버림 */
    CHECK_EQ_INT(o.res.client_keep_alive, 0);
    CHECK_EQ_STR(strstr(o.client.data, "\r\n\r\n") + 4, body); /* 그대로 통과 */
    buf_free(&o.client);

    /* HTTP/1.0 클라이언트는 chunked 를 못 읽는다 → 502 (아무것도 안 보냄) */
    relay("GET / HTTP/1.0\r\n\r\n", backend, 1, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_EBAD);
    CHECK_EQ_SIZE(o.client.len, 0);
    buf_free(&o.client);
}

static void test_relay_no_body_cases(void)
{
    relay_out_t o;

    /* HEAD: 백엔드가 알려 준 CL 은 유지하되 본문은 읽지 않는다 */
    relay("HEAD / HTTP/1.1\r\nHost: h\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", 0,
          2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_OK);
    CHECK(strstr(o.client.data, "Content-Length: 1000\r\n") != NULL);
    CHECK_EQ_STR(strstr(o.client.data, "\r\n\r\n") + 4, "");
    CHECK_EQ_INT(o.res.client_keep_alive, 1);
    buf_free(&o.client);

    relay(GET, "HTTP/1.1 204 No Content\r\n\r\n", 0, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_OK);
    CHECK_EQ_INT(o.res.status, 204);
    CHECK_EQ_INT(o.res.client_keep_alive, 1); /* EOF 를 기다리지 않음 */
    buf_free(&o.client);

    /* 1xx 중간 응답은 버리고 최종 응답만 */
    relay(GET, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 0, 2000,
          &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_OK);
    CHECK(strstr(o.client.data, "100 Continue") == NULL);
    CHECK_EQ_STR(strstr(o.client.data, "\r\n\r\n") + 4, "ok");
    buf_free(&o.client);
}

static void test_relay_errors(void)
{
    relay_out_t o;
    long t0;

    t0 = ms_now();
    relay(GET, "", 0, 300, &o); /* 백엔드가 아무 말도 안 함 */
    CHECK_EQ_INT(o.rc, UPSTREAM_ETIMEOUT);
    CHECK(ms_now() - t0 < 1500);
    CHECK_EQ_SIZE(o.client.len, 0); /* 클라이언트에 아무것도 안 보냈으니 504 를 보낼 수 있다 */
    buf_free(&o.client);

    relay(GET, "HTTP/1.1 200 OK\r\nContent-Le", 1, 2000, &o); /* 헤더 중간 EOF */
    CHECK_EQ_INT(o.rc, UPSTREAM_EBAD);
    CHECK_EQ_SIZE(o.client.len, 0);
    buf_free(&o.client);

    relay(GET, "SSH-2.0-OpenSSH\r\n\r\n", 1, 2000, &o); /* HTTP 가 아님 */
    CHECK_EQ_INT(o.rc, UPSTREAM_EBAD);
    buf_free(&o.client);

    relay(GET, "HTTP/1.1 101 Switching Protocols\r\n\r\n", 1, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_EBAD);
    buf_free(&o.client);

    /* CL 을 다 채우기 전 EOF: 헤더는 이미 나갔으므로 EPARTIAL → 호출자가 연결을 끊는다 */
    relay(GET, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", 1, 2000, &o);
    CHECK_EQ_INT(o.rc, UPSTREAM_EPARTIAL);
    CHECK_EQ_INT(o.res.client_keep_alive, 0);
    CHECK(o.client.len > 0);
    buf_free(&o.client);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_hop_by_hop);
    RUN_TEST(test_build_request);
    RUN_TEST(test_parse_response_head);
    RUN_TEST(test_connect);
    RUN_TEST(test_relay_content_length_keeps_alive);
    RUN_TEST(test_relay_until_eof_closes);
    RUN_TEST(test_relay_chunked);
    RUN_TEST(test_relay_no_body_cases);
    RUN_TEST(test_relay_errors);
    TEST_END();
}
