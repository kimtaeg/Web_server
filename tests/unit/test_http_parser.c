/*
 * test_http_parser.c — http_parser 단위 테스트.
 *
 * 핵심 장치: parse_all_modes()
 *   같은 입력을 (0) 한 번에, (1) 1바이트씩, (2..6) 고정 시드 무작위 분할로 넣고,
 *   결과(결과값, 상태 코드, consumed, req 의 모든 필드)를 문자열 하나로 정규화해서 비교한다.
 *   분할 피딩은 buf_t 에 append 하므로 realloc 으로 주소가 바뀌어도 맞는지까지 검증된다.
 *   모든 정상/오류 케이스가 이 장치를 거치므로 "쪼개 넣어도 결과가 같다"가 케이스 전체에 대해 증명된다.
 */
#include "test.h"

#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "buf.h"
#include "http_parser.h"

#define WANT_OK 0
#define WANT_MORE (-1)

/* ------------------------------------------------------------------ */
/* 하네스                                                              */
/* ------------------------------------------------------------------ */

static void describe(buf_t *d, parse_result_t r, int status, size_t used, const http_request_t *q)
{
    size_t i;

    buf_appendf(d, "r=%d status=%d consumed=%zu", (int)r, status, used);
    if (r != PARSE_OK)
        return;
    buf_appendf(d, " method=%s target=[%.*s] path=[%.*s]", http_method_name(q->method),
                (int)q->target_len, q->target, (int)q->path_len, q->path);
    if (q->query != NULL)
        buf_appendf(d, " query=[%.*s]", (int)q->query_len, q->query);
    else
        buf_appendf(d, " query=NULL");
    buf_appendf(d, " abs=%d", q->is_absolute);
    if (q->authority != NULL)
        buf_appendf(d, " authority=[%.*s]", (int)q->authority_len, q->authority);
    buf_appendf(d, " minor=%d ka=%d cl=%zu nh=%zu", q->version_minor, q->keep_alive,
                q->content_length, q->header_count);
    for (i = 0; i < q->header_count; i++)
        buf_appendf(d, " H[%.*s]=[%.*s]", (int)q->headers[i].name_len, q->headers[i].name,
                    (int)q->headers[i].value_len, q->headers[i].value);
    buf_appendf(d, " body=[%.*s]", (int)q->body_len, q->body ? q->body : "");
}

static unsigned lcg(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fffu;
}

/* mode 0: 한 번에, 1: 1바이트씩, 2 이상: 시드 = mode 인 무작위 분할(1~64바이트). */
static void run_mode(const char *raw, size_t n, const http_limits_t *lim, int mode, buf_t *desc)
{
    http_parser_t p;
    http_request_t req;
    buf_t in;
    size_t fed = 0;
    size_t used = 0;
    parse_result_t r = PARSE_NEED_MORE;
    uint32_t seed = (uint32_t)mode;

    http_parser_init(&p, lim);
    buf_init(&in, 0, n + 1);
    while (fed < n) {
        size_t step = mode == 0 ? n : mode == 1 ? 1 : 1 + lcg(&seed) % 64;
        if (step > n - fed)
            step = n - fed;
        buf_append(&in, raw + fed, step);
        fed += step;
        r = http_parse(&p, in.data, in.len, &req, &used);
        if (r != PARSE_NEED_MORE)
            break;
    }
    describe(desc, r, http_parser_error_status(&p), used, &req);
    buf_free(&in);
}

/* 모든 분할 모드의 결과가 한 번에 넣은 결과와 같은지 검사하고, 그 결과를 whole 에 남긴다. */
static void parse_all_modes(const char *name, const char *raw, size_t n, const http_limits_t *lim,
                            buf_t *whole)
{
    int mode;

    buf_init(whole, 0, 1 << 20);
    run_mode(raw, n, lim, 0, whole);
    for (mode = 1; mode <= 6; mode++) {
        buf_t d;
        int same;

        buf_init(&d, 0, 1 << 20);
        run_mode(raw, n, lim, mode, &d);
        same = d.len == whole->len && memcmp(d.data, whole->data, d.len) == 0;
        if (!same)
            fprintf(stderr, "    [%s] mode %d 결과 불일치\n      whole: %.300s\n      split: %.300s\n",
                    name, mode, whole->data, d.data);
        CHECK(same);
        buf_free(&d);
    }
}

/* 기대 결과(WANT_OK / WANT_MORE / 상태 코드)를 모든 모드에서 검사. */
static void expect_all_modes(const char *name, const char *raw, size_t n, const http_limits_t *lim,
                             int want)
{
    buf_t whole;
    char prefix[64];

    parse_all_modes(name, raw, n, lim, &whole);
    if (want == WANT_OK)
        snprintf(prefix, sizeof prefix, "r=%d status=0 ", PARSE_OK);
    else if (want == WANT_MORE)
        snprintf(prefix, sizeof prefix, "r=%d status=0 ", PARSE_NEED_MORE);
    else
        snprintf(prefix, sizeof prefix, "r=%d status=%d ", PARSE_ERROR, want);
    if (strncmp(whole.data, prefix, strlen(prefix)) != 0)
        fprintf(stderr, "    [%s] 기대 \"%s...\", 실제 \"%.200s\"\n", name, prefix, whole.data);
    CHECK(strncmp(whole.data, prefix, strlen(prefix)) == 0);
    buf_free(&whole);
}

/* raw 를 한 번에 파싱 (필드 검사용, req 포인터는 raw 를 가리킴). */
static parse_result_t parse_once(const char *raw, const http_limits_t *lim, http_request_t *req,
                                 size_t *used, int *status)
{
    http_parser_t p;
    parse_result_t r;

    http_parser_init(&p, lim);
    r = http_parse(&p, raw, strlen(raw), req, used);
    *status = http_parser_error_status(&p);
    return r;
}

static int span_eq(const char *p, size_t n, const char *want)
{
    return p != NULL && strlen(want) == n && memcmp(p, want, n) == 0;
}

/* 동적으로 큰 입력을 만드는 도우미. 반환값은 free 해야 한다. */
static char *build(const char *head, char fill, size_t fill_n, const char *tail)
{
    size_t hl = strlen(head), tl = strlen(tail);
    char *s = malloc(hl + fill_n + tl + 1);

    if (s == NULL)
        abort();
    memcpy(s, head, hl);
    memset(s + hl, fill, fill_n);
    memcpy(s + hl + fill_n, tail, tl + 1);
    return s;
}

/* ------------------------------------------------------------------ */
/* 정상 요청                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *raw;
    http_method_t method;
    const char *path;
    const char *query; /* NULL = 쿼리 없음 */
    int minor;
    int keep_alive;
    size_t content_length;
    const char *body;
    size_t consumed; /* 0 이면 strlen(raw) */
} ok_case_t;

static const ok_case_t ok_cases[] = {
    {"simple GET", "GET / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_GET, "/", NULL, 1, 1, 0, "", 0},
    {"path + query", "GET /search?q=c+lang&x=1 HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_GET, "/search",
     "q=c+lang&x=1", 1, 1, 0, "", 0},
    {"empty query", "GET /p? HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_GET, "/p", "", 1, 1, 0, "", 0},
    {"second ? stays in query", "GET /p?a=1?b HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_GET, "/p",
     "a=1?b", 1, 1, 0, "", 0},
    {"HEAD 1.0 no Host", "HEAD /x HTTP/1.0\r\n\r\n", HTTP_HEAD, "/x", NULL, 0, 0, 0, "", 0},
    {"POST with body", "POST /cgi HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello", HTTP_POST,
     "/cgi", NULL, 1, 1, 5, "hello", 0},
    {"POST CL 0", "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", HTTP_POST, "/x", NULL,
     1, 1, 0, "", 0},
    {"duplicate equal CL", "PUT /f HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
     HTTP_PUT, "/f", NULL, 1, 1, 3, "abc", 0},
    {"CL leading zeros", "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 007\r\n\r\n1234567", HTTP_POST,
     "/", NULL, 1, 1, 7, "1234567", 0},
    {"GET with body", "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nhi", HTTP_GET, "/", NULL,
     1, 1, 2, "hi", 0},
    {"DELETE", "DELETE /r/1 HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_DELETE, "/r/1", NULL, 1, 1, 0, "", 0},
    {"OPTIONS *", "OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_OPTIONS, "*", NULL, 1, 1, 0, "", 0},
    {"PATCH", "PATCH /j HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\n{}", HTTP_PATCH, "/j", NULL,
     1, 1, 2, "{}", 0},
    {"leading blank lines", "\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_GET, "/", NULL, 1, 1, 0,
     "", 0},
    {"pipelined leftover kept", "GET /a HTTP/1.1\r\nHost: a\r\n\r\nGET /b", HTTP_GET, "/a", NULL, 1, 1,
     0, "", 28},
    {"body then leftover", "POST /a HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\nXYZ", HTTP_POST,
     "/a", NULL, 1, 1, 1, "X", 49},
    {"header OWS, mixed case name", "GET / HTTP/1.1\r\nhOsT:   a  \r\nX-Tab:\tv1\t v2 \t\r\n\r\n",
     HTTP_GET, "/", NULL, 1, 1, 0, "", 0},
    {"obs-text in value", "GET / HTTP/1.1\r\nHost: a\r\nX-Name: caf\xc3\xa9\r\n\r\n", HTTP_GET, "/",
     NULL, 1, 1, 0, "", 0},
    {"empty header value", "GET / HTTP/1.1\r\nHost: a\r\nX-Empty:\r\n\r\n", HTTP_GET, "/", NULL, 1, 1,
     0, "", 0},
};

static void test_ok_cases(void)
{
    size_t i;

    for (i = 0; i < sizeof ok_cases / sizeof ok_cases[0]; i++) {
        const ok_case_t *c = &ok_cases[i];
        http_request_t req;
        size_t used = 0;
        int status = 0;
        parse_result_t r;

        expect_all_modes(c->name, c->raw, strlen(c->raw), NULL, WANT_OK);

        r = parse_once(c->raw, NULL, &req, &used, &status);
        CHECK_EQ_INT(r, PARSE_OK);
        if (r != PARSE_OK) {
            fprintf(stderr, "    [%s] status=%d\n", c->name, status);
            continue;
        }
        CHECK_EQ_INT(req.method, c->method);
        CHECK(span_eq(req.path, req.path_len, c->path));
        if (c->query == NULL)
            CHECK(req.query == NULL && req.query_len == 0);
        else
            CHECK(span_eq(req.query, req.query_len, c->query));
        CHECK_EQ_INT(req.version_minor, c->minor);
        CHECK_EQ_INT(req.keep_alive, c->keep_alive);
        CHECK_EQ_SIZE(req.content_length, c->content_length);
        CHECK_EQ_SIZE(req.body_len, strlen(c->body));
        if (req.body_len > 0)
            CHECK(span_eq(req.body, req.body_len, c->body));
        CHECK_EQ_SIZE(used, c->consumed ? c->consumed : strlen(c->raw));
        CHECK_EQ_INT(req.is_absolute, 0);
    }
}

static void test_absolute_form(void)
{
    static const struct {
        const char *raw;
        const char *authority;
        const char *path;
        const char *query;
    } cases[] = {
        {"GET http://example.com:8080/a/b?x=1 HTTP/1.1\r\nHost: example.com:8080\r\n\r\n",
         "example.com:8080", "/a/b", "x=1"},
        {"GET http://example.com HTTP/1.1\r\nHost: example.com\r\n\r\n", "example.com", "/", NULL},
        {"GET HTTP://Example.com?q HTTP/1.1\r\nHost: e\r\n\r\n", "Example.com", "/", "q"},
        {"GET http://h/ HTTP/1.0\r\n\r\n", "h", "/", NULL},
        {"GET HTTPS://secure.example:8443/p HTTP/1.1\r\nHost: s\r\n\r\n", "secure.example:8443", "/p",
         NULL},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request_t req;
        size_t used;
        int status;

        expect_all_modes("absolute-form", cases[i].raw, strlen(cases[i].raw), NULL, WANT_OK);
        REQUIRE(parse_once(cases[i].raw, NULL, &req, &used, &status) == PARSE_OK);
        CHECK_EQ_INT(req.is_absolute, 1);
        CHECK_EQ_INT(req.is_https, strncasecmp(cases[i].raw + 4, "https", 5) == 0);
        CHECK(span_eq(req.authority, req.authority_len, cases[i].authority));
        CHECK(span_eq(req.path, req.path_len, cases[i].path));
        if (cases[i].query == NULL)
            CHECK(req.query == NULL);
        else
            CHECK(span_eq(req.query, req.query_len, cases[i].query));
    }
}

static void test_header_lookup(void)
{
    const char *raw = "GET / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Content-Type:  text/plain \r\n"
                      "X-Multi: 1\r\n"
                      "x-multi: 2\r\n"
                      "\r\n";
    http_request_t req;
    size_t used, vlen = 99;
    int status;
    const char *v;

    REQUIRE(parse_once(raw, NULL, &req, &used, &status) == PARSE_OK);
    CHECK_EQ_SIZE(req.header_count, 4);
    CHECK(span_eq(req.headers[1].name, req.headers[1].name_len, "Content-Type")); /* 원본 대소문자 보존 */

    v = http_header_get(&req, "content-type", &vlen);
    CHECK(span_eq(v, vlen, "text/plain"));
    v = http_header_get(&req, "HOST", &vlen);
    CHECK(span_eq(v, vlen, "example.com"));
    v = http_header_get(&req, "X-MULTI", &vlen);
    CHECK(span_eq(v, vlen, "1")); /* 첫 번째 */
    CHECK(http_header_get(&req, "Missing", &vlen) == NULL);
    CHECK(http_header_get(&req, "Content", &vlen) == NULL); /* 접두사만 같으면 불일치 */
    CHECK(http_header_get(&req, "Content-Type-X", &vlen) == NULL);
    CHECK(http_header_get(&req, "host", NULL) != NULL); /* value_len NULL 허용 */
}

static void test_method_name(void)
{
    CHECK_EQ_STR(http_method_name(HTTP_GET), "GET");
    CHECK_EQ_STR(http_method_name(HTTP_HEAD), "HEAD");
    CHECK_EQ_STR(http_method_name(HTTP_POST), "POST");
    CHECK_EQ_STR(http_method_name(HTTP_PUT), "PUT");
    CHECK_EQ_STR(http_method_name(HTTP_DELETE), "DELETE");
    CHECK_EQ_STR(http_method_name(HTTP_OPTIONS), "OPTIONS");
    CHECK_EQ_STR(http_method_name(HTTP_PATCH), "PATCH");
}

/* ------------------------------------------------------------------ */
/* 오류 정책 (테이블 기반)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *raw;
    int want; /* 상태 코드, WANT_OK, WANT_MORE */
} err_case_t;

#define H "Host: a\r\n"

static const err_case_t err_cases[] = {
    /* ---- 400: 요청줄 ---- */
    {"400 no SP", "GET\r\n\r\n", 400},
    {"400 HTTP/0.9 style", "GET /\r\n\r\n", 400},
    {"400 empty method", " / HTTP/1.1\r\n" H "\r\n", 400},
    {"400 double SP", "GET  / HTTP/1.1\r\n" H "\r\n", 400},
    {"400 non-tchar in method", "G(T / HTTP/1.1\r\n" H "\r\n", 400},
    {"400 relative target", "GET index.html HTTP/1.1\r\n" H "\r\n", 400},
    {"400 * with GET", "GET * HTTP/1.1\r\n" H "\r\n", 400},
    {"400 * with suffix", "OPTIONS *x HTTP/1.1\r\n" H "\r\n", 400},
    {"OK https absolute (형식상 허용, fproxy 가 501)", "GET https://a/ HTTP/1.1\r\n" H "\r\n", WANT_OK},
    {"400 https 빈 authority", "GET https:///x HTTP/1.1\r\n" H "\r\n", 400},
    {"400 ftp 스킴", "GET ftp://a/ HTTP/1.1\r\n" H "\r\n", 400},
    {"400 empty authority", "GET http:///x HTTP/1.1\r\n" H "\r\n", 400},
    {"400 CTL in target", "GET /a\x01" "b HTTP/1.1\r\n" H "\r\n", 400},
    {"400 DEL in target", "GET /a\x7f HTTP/1.1\r\n" H "\r\n", 400},
    {"400 non-ASCII in target", "GET /caf\xc3\xa9 HTTP/1.1\r\n" H "\r\n", 400},
    {"400 trailing SP after version", "GET / HTTP/1.1 \r\n" H "\r\n", 400},
    {"400 lowercase version", "GET / http/1.1\r\n" H "\r\n", 400},
    {"400 version without minor", "GET / HTTP/1\r\n" H "\r\n", 400},
    {"400 version 2-digit minor", "GET / HTTP/1.10\r\n" H "\r\n", 400},
    {"400 version garbage", "GET / HTTP/x.y\r\n" H "\r\n", 400},
    /* ---- 400: 줄바꿈 ---- */
    {"400 bare LF in request line", "GET / HTTP/1.1\n" H "\r\n", 400},
    {"400 bare LF in header", "GET / HTTP/1.1\r\nHost: a\n\r\n", 400},
    {"400 bare LF before request", "\nGET / HTTP/1.1\r\n" H "\r\n", 400},
    {"400 CR without LF before request", "\rGET / HTTP/1.1\r\n" H "\r\n", 400},
    {"400 too many blank lines", "\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\nGET / HTTP/1.1\r\n" H "\r\n", 400},
    {"OK 8 blank lines", "\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\nGET / HTTP/1.1\r\n" H "\r\n", WANT_OK},
    /* ---- 400: 헤더 형식 ---- */
    {"400 header without colon", "GET / HTTP/1.1\r\n" H "NoColon\r\n\r\n", 400},
    {"400 space before colon", "GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400},
    {"400 empty header name", "GET / HTTP/1.1\r\n" H ": v\r\n\r\n", 400},
    {"400 obs-fold", "GET / HTTP/1.1\r\n" H "X: 1\r\n  more\r\n\r\n", 400},
    {"400 tab obs-fold", "GET / HTTP/1.1\r\n" H "X: 1\r\n\tmore\r\n\r\n", 400},
    {"400 CTL in header value", "GET / HTTP/1.1\r\n" H "X: a\x01" "b\r\n\r\n", 400},
    {"400 DEL in header value", "GET / HTTP/1.1\r\n" H "X: a\x7f\r\n\r\n", 400},
    {"400 CR in header value", "GET / HTTP/1.1\r\n" H "X: a\rb\r\n\r\n", 400},
    /* ---- 400: Content-Length ---- */
    {"400 CL not digits", "POST / HTTP/1.1\r\n" H "Content-Length: abc\r\n\r\n", 400},
    {"400 CL negative", "POST / HTTP/1.1\r\n" H "Content-Length: -1\r\n\r\n", 400},
    {"400 CL plus sign", "POST / HTTP/1.1\r\n" H "Content-Length: +5\r\n\r\n", 400},
    {"400 CL empty", "POST / HTTP/1.1\r\n" H "Content-Length:\r\n\r\n", 400},
    {"400 CL inner space", "POST / HTTP/1.1\r\n" H "Content-Length: 1 2\r\n\r\n", 400},
    {"400 CL list", "POST / HTTP/1.1\r\n" H "Content-Length: 5, 5\r\n\r\n", 400},
    {"400 CL overflow", "POST / HTTP/1.1\r\n" H "Content-Length: 184467440737095516160\r\n\r\n", 400},
    {"400 CL conflicting", "POST / HTTP/1.1\r\n" H "Content-Length: 3\r\nContent-Length: 4\r\n\r\nabcd",
     400},
    /* ---- 400: Host ---- */
    {"400 HTTP/1.1 without Host", "GET / HTTP/1.1\r\n\r\n", 400},
    {"400 Host twice", "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400},
    {"OK HTTP/1.0 without Host", "GET / HTTP/1.0\r\n\r\n", WANT_OK},
    /* ---- 400: CL + TE (요청 스머글링 방지) ---- */
    {"400 CL then TE", "POST / HTTP/1.1\r\n" H "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello",
     400},
    {"400 TE then CL", "POST / HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhello",
     400},
    {"400 CL+TE beats 413", "POST / HTTP/1.1\r\n" H "Content-Length: 99999999\r\nTransfer-Encoding: chunked\r\n\r\n",
     400},
    /* ---- 411 ---- */
    {"411 POST without CL", "POST / HTTP/1.1\r\n" H "\r\n", 411},
    {"411 PUT without CL", "PUT / HTTP/1.1\r\n" H "\r\n", 411},
    {"411 PATCH without CL", "PATCH / HTTP/1.1\r\n" H "\r\n", 411},
    {"411 HTTP/1.0 POST without CL", "POST / HTTP/1.0\r\n\r\n", 411},
    {"411 chunked TE", "POST / HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n\r\n", 411},
    {"OK GET without CL", "GET / HTTP/1.1\r\n" H "\r\n", WANT_OK},
    {"OK DELETE without CL", "DELETE / HTTP/1.1\r\n" H "\r\n", WANT_OK},
    /* ---- 413 (기본 1MB) ---- */
    {"413 CL over 1MB", "POST / HTTP/1.1\r\n" H "Content-Length: 1048577\r\n\r\n", 413},
    {"MORE CL exactly 1MB waits for body", "POST / HTTP/1.1\r\n" H "Content-Length: 1048576\r\n\r\n",
     WANT_MORE},
    /* ---- 501 ---- */
    {"501 unknown method", "BREW /pot HTTP/1.1\r\n" H "\r\n", 501},
    {"501 CONNECT", "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n", 501},
    {"501 TRACE", "TRACE / HTTP/1.1\r\n" H "\r\n", 501},
    {"501 lowercase method", "get / HTTP/1.1\r\n" H "\r\n", 501},
    {"501 method too long", "ABCDEFGHIJKLMNOPQ / HTTP/1.1\r\n" H "\r\n", 501},
    /* ---- 505 ---- */
    {"505 HTTP/2.0", "GET / HTTP/2.0\r\n" H "\r\n", 505},
    {"505 HTTP/1.2", "GET / HTTP/1.2\r\n" H "\r\n", 505},
    {"505 HTTP/0.9", "GET / HTTP/0.9\r\n" H "\r\n", 505},
    /* ---- 판정 순서: 왼쪽에서 오른쪽 ---- */
    {"order 501 before 505", "BREW / HTTP/2.0\r\n\r\n", 501},
    {"order target 400 before 505", "GET x HTTP/2.0\r\n\r\n", 400},
    {"order 505 before missing Host", "GET / HTTP/2.0\r\n\r\n", 505},
    {"order header 400 before 411", "POST / HTTP/1.1\r\n" H "Bad\r\n\r\n", 400},
    /* ---- 미완성 ---- */
    {"MORE no blank line yet", "GET / HTTP/1.1\r\n" H, WANT_MORE},
    {"MORE CR without LF yet", "GET / HTTP/1.1\r\n" H "\r", WANT_MORE},
    {"MORE partial body", "POST / HTTP/1.1\r\n" H "Content-Length: 10\r\n\r\nhello", WANT_MORE},
    {"MORE only blank lines", "\r\n\r\n", WANT_MORE},
};

static void test_error_table(void)
{
    size_t i;

    for (i = 0; i < sizeof err_cases / sizeof err_cases[0]; i++)
        expect_all_modes(err_cases[i].name, err_cases[i].raw, strlen(err_cases[i].raw), NULL,
                         err_cases[i].want);
}

/* 작은 상한으로 경계값(정확히 상한 = 허용, 상한+1 = 거부)을 검사. */
static void test_limits_boundaries(void)
{
    const http_limits_t lim = {16, 64, 3, 10};
    static const err_case_t cases[] = {
        {"414 target 17 > 16", "GET /0123456789abcdef HTTP/1.1\r\n" H "\r\n", 414},
        {"OK target exactly 16", "GET /0123456789abcde HTTP/1.1\r\n" H "\r\n", WANT_OK},
        {"414 absolute target too long", "GET http://abcdefghijk/ HTTP/1.1\r\n" H "\r\n", 414},
        {"413 CL 11 > 10", "POST / HTTP/1.1\r\n" H "Content-Length: 11\r\n\r\n", 413},
        {"OK CL exactly 10", "POST / HTTP/1.1\r\n" H "Content-Length: 10\r\n\r\n0123456789", WANT_OK},
        {"431 4 headers > 3", "GET / HTTP/1.1\r\n" H "A: 1\r\nB: 2\r\nC: 3\r\n\r\n", 431},
        {"OK exactly 3 headers", "GET / HTTP/1.1\r\n" H "A: 1\r\nB: 2\r\n\r\n", WANT_OK},
    };
    size_t i;
    char *s;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        expect_all_modes(cases[i].name, cases[i].raw, strlen(cases[i].raw), &lim, cases[i].want);

    /* 헤더 영역 = "Host: a\r\n"(9) + "X: "(3) + 값 + "\r\n"(2) + 빈 줄(2) → 값 48 이면 정확히 64 */
    s = build("GET / HTTP/1.1\r\nHost: a\r\nX: ", 'v', 48, "\r\n\r\n");
    expect_all_modes("OK header bytes exactly 64", s, strlen(s), &lim, WANT_OK);
    free(s);
    s = build("GET / HTTP/1.1\r\nHost: a\r\nX: ", 'v', 49, "\r\n\r\n");
    expect_all_modes("431 header bytes 65 > 64", s, strlen(s), &lim, 431);
    free(s);
}

/* 기본 상한(8KB URI, 16KB 헤더, 100개)에서의 경계값. */
static void test_default_limits(void)
{
    char *s;
    buf_t b;
    int i;

    /* 대상 = "/" + 8191 = 8192 바이트 → 허용, 8193 → 414 */
    s = build("GET /", 'a', 8191, " HTTP/1.1\r\nHost: a\r\n\r\n");
    expect_all_modes("OK target 8192", s, strlen(s), NULL, WANT_OK);
    free(s);
    s = build("GET /", 'a', 8192, " HTTP/1.1\r\nHost: a\r\n\r\n");
    expect_all_modes("414 target 8193", s, strlen(s), NULL, 414);
    free(s);

    /* 헤더 하나가 너무 커서 16KB 초과 */
    s = build("GET / HTTP/1.1\r\nHost: a\r\nX-Big: ", 'v', 16384, "\r\n\r\n");
    expect_all_modes("431 header section > 16KB", s, strlen(s), NULL, 431);
    free(s);

    /* 100개(Host + X-1..X-99) 허용, 101개 431 */
    for (i = 99; i <= 100; i++) {
        int k;

        buf_init(&b, 0, 1 << 16);
        buf_appendf(&b, "GET / HTTP/1.1\r\nHost: a\r\n");
        for (k = 1; k <= i; k++)
            buf_appendf(&b, "X-%d: %d\r\n", k, k);
        buf_appendf(&b, "\r\n");
        if (i == 99)
            expect_all_modes("OK 100 headers", b.data, b.len, NULL, WANT_OK);
        else
            expect_all_modes("431 101 headers", b.data, b.len, NULL, 431);
        buf_free(&b);
    }
}

/*
 * 줄이 끝나지 않아도 상한을 넘는 즉시 오류가 나야 한다.
 * (끝없이 보내는 요청줄/헤더로 메모리를 고갈시키는 공격 방지)
 * 입력에 줄 끝이 없으므로, 조기 판정을 안 하면 결과가 NEED_MORE 가 된다.
 */
static void test_early_rejection(void)
{
    char *s;

    s = build("GET /", 'a', 9000, "");
    expect_all_modes("414 before request line ends", s, strlen(s), NULL, 414);
    free(s);

    s = build("GET / HTTP/1.1\r\nHost: a\r\nX: ", 'v', 17000, "");
    expect_all_modes("431 before header line ends", s, strlen(s), NULL, 431);
    free(s);

    s = build("GET / HTTP/1.1", ' ', 9000, "");
    expect_all_modes("400 junk after version before line ends", s, strlen(s), NULL, 400);
    free(s);

    s = build("", 'A', 9000, "");
    expect_all_modes("501 endless method token", s, strlen(s), NULL, 501);
    free(s);

    /*
     * 긴 대상 + bare LF: 쪼개 넣으면 LF 가 오기 전에 414 가 난다.
     * 한 번에 넣었을 때 LF 검사를 먼저 하면 400 이 되어 분할 무관성이 깨진다.
     * → 줄 끝 검사는 줄의 가장 오른쪽이므로 가장 마지막이어야 한다.
     */
    s = build("GET /", 'a', 9000, "\n");
    expect_all_modes("414 long target with bare LF", s, strlen(s), NULL, 414);
    free(s);
}

/* ------------------------------------------------------------------ */
/* keep-alive                                                         */
/* ------------------------------------------------------------------ */

static void test_keep_alive(void)
{
    static const struct {
        const char *name;
        const char *raw;
        int want;
    } cases[] = {
        {"1.1 default", "GET / HTTP/1.1\r\n" H "\r\n", 1},
        {"1.1 close", "GET / HTTP/1.1\r\n" H "Connection: close\r\n\r\n", 0},
        {"1.1 CLOSE (case)", "GET / HTTP/1.1\r\n" H "Connection: CLOSE\r\n\r\n", 0},
        {"1.1 explicit keep-alive", "GET / HTTP/1.1\r\n" H "Connection: keep-alive\r\n\r\n", 1},
        {"1.1 token list with close", "GET / HTTP/1.1\r\n" H "Connection: keep-alive, close\r\n\r\n", 0},
        {"1.1 close in 2nd header", "GET / HTTP/1.1\r\n" H "Connection: foo\r\nConnection: close\r\n\r\n", 0},
        {"1.1 'closed' is not close", "GET / HTTP/1.1\r\n" H "Connection: closed\r\n\r\n", 1},
        {"1.0 default", "GET / HTTP/1.0\r\n\r\n", 0},
        {"1.0 keep-alive", "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 1},
        {"1.0 Keep-Alive (case)", "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", 1},
        {"1.0 token list", "GET / HTTP/1.0\r\nConnection: Upgrade,  keep-alive \r\n\r\n", 1},
        {"1.0 keep-alive + close", "GET / HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n", 0},
        {"1.0 'keep-alivex' is not", "GET / HTTP/1.0\r\nConnection: keep-alivex\r\n\r\n", 0},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request_t req;
        size_t used;
        int status;

        expect_all_modes(cases[i].name, cases[i].raw, strlen(cases[i].raw), NULL, WANT_OK);
        REQUIRE(parse_once(cases[i].raw, NULL, &req, &used, &status) == PARSE_OK);
        if (req.keep_alive != cases[i].want)
            fprintf(stderr, "    [%s]\n", cases[i].name);
        CHECK_EQ_INT(req.keep_alive, cases[i].want);
    }
}

/* ------------------------------------------------------------------ */
/* 파이프라이닝                                                        */
/* ------------------------------------------------------------------ */

/*
 * 요청 3개를 이어 붙인 스트림을 여러 방식으로 쪼개 넣는다.
 * OK 마다 consume → init → 남은 바이트로 즉시 재파싱. 모든 모드에서 같은 순서/내용이어야 한다.
 */
static void run_pipeline(const char *raw, size_t n, int mode, buf_t *log)
{
    http_parser_t p;
    http_request_t req;
    buf_t in;
    size_t fed = 0, used;
    uint32_t seed = (uint32_t)mode;
    parse_result_t r;

    http_parser_init(&p, NULL);
    buf_init(&in, 0, n + 1);
    while (fed < n) {
        size_t step = mode == 0 ? n : mode == 1 ? 1 : 1 + lcg(&seed) % 64;
        if (step > n - fed)
            step = n - fed;
        buf_append(&in, raw + fed, step);
        fed += step;
        while ((r = http_parse(&p, in.data, in.len, &req, &used)) == PARSE_OK) {
            buf_appendf(log, "%.*s:%.*s:%d|", (int)req.path_len, req.path, (int)req.body_len,
                        req.body ? req.body : "", req.keep_alive);
            buf_consume(&in, used);
            http_parser_init(&p, NULL);
        }
        if (r == PARSE_ERROR) {
            buf_appendf(log, "ERROR %d", http_parser_error_status(&p));
            break;
        }
    }
    buf_appendf(log, "left=%zu", in.len);
    buf_free(&in);
}

static void test_pipelining(void)
{
    const char *raw = "POST /a HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\nabc"
                      "\r\n" /* 본문 뒤 여분의 CRLF 도 허용 */
                      "GET /b HTTP/1.1\r\nHost: a\r\n\r\n"
                      "GET /c HTTP/1.0\r\n\r\n"
                      "GET /d HT"; /* 미완성 네 번째 요청은 남아 있어야 한다 */
    const char *want = "/a:abc:1|/b::1|/c::0|left=9";
    int mode;

    for (mode = 0; mode <= 6; mode++) {
        buf_t log;

        buf_init(&log, 0, 4096);
        run_pipeline(raw, strlen(raw), mode, &log);
        if (strcmp(log.data, want) != 0)
            fprintf(stderr, "    mode %d: %s\n", mode, log.data);
        CHECK_EQ_STR(log.data, want);
        buf_free(&log);
    }
}

/* ------------------------------------------------------------------ */
/* 기타 계약                                                           */
/* ------------------------------------------------------------------ */

static void test_error_is_sticky(void)
{
    http_parser_t p;
    http_request_t req;
    size_t used = 123;
    const char *bad = "BREW / HTTP/1.1\r\n";
    const char *more = "BREW / HTTP/1.1\r\nHost: a\r\n\r\n";

    http_parser_init(&p, NULL);
    CHECK_EQ_INT(http_parse(&p, bad, strlen(bad), &req, &used), PARSE_ERROR);
    CHECK_EQ_INT(http_parser_error_status(&p), 501);
    CHECK_EQ_SIZE(used, 0);
    CHECK_EQ_INT(http_parse(&p, more, strlen(more), &req, &used), PARSE_ERROR);
    CHECK_EQ_INT(http_parser_error_status(&p), 501);
}

static void test_need_more_contract(void)
{
    http_parser_t p;
    http_request_t req;
    size_t used = 123;
    const char *part = "GET / HTTP/1.1\r\nHo";

    http_parser_init(&p, NULL);
    CHECK_EQ_INT(http_parse(&p, part, strlen(part), &req, &used), PARSE_NEED_MORE);
    CHECK_EQ_SIZE(used, 0);
    CHECK_EQ_INT(http_parser_error_status(&p), 0);

    http_parser_init(&p, NULL);
    CHECK_EQ_INT(http_parse(&p, part, 0, &req, &used), PARSE_NEED_MORE); /* 빈 입력 */
}

/* 상한 설정이 배열 크기(HTTP_MAX_HEADERS)를 넘어도 배열 밖에 쓰지 않는다. */
static void test_max_headers_clamped(void)
{
    const http_limits_t lim = {8192, 1 << 20, 100000, 1024};
    buf_t b;
    int i;

    buf_init(&b, 0, 1 << 16);
    buf_appendf(&b, "GET / HTTP/1.1\r\nHost: a\r\n");
    for (i = 1; i <= HTTP_MAX_HEADERS; i++)
        buf_appendf(&b, "X-%d: %d\r\n", i, i);
    buf_appendf(&b, "\r\n");
    expect_all_modes("431 clamp to HTTP_MAX_HEADERS", b.data, b.len, &lim, 431);
    buf_free(&b);
}

/*
 * 무작위 변형 입력에 대한 분할 무관성.
 * 정상 요청의 몇 바이트를 "파서가 민감한 문자"로 바꾸거나 잘라낸 입력 2000개를 만들어,
 * 어떤 결과(OK/오류/NEED_MORE)가 나오든 모든 분할 모드에서 같아야 한다.
 * make asan 에서는 이상 입력에서의 메모리 오류도 함께 검사된다.
 */
static void test_fuzz_split_invariance(void)
{
    static const char *bases[] = {
        "POST /cgi/a?x=1 HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbody",
        "\r\nGET http://h:1/p?q HTTP/1.0\r\nConnection: keep-alive\r\nX: \t v \r\n\r\n",
        "OPTIONS * HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
    };
    static const char alphabet[] = "\r\n :\t/?aZ0*\x01\x7f\x80";
    const http_limits_t lim = {24, 96, 4, 8}; /* 작은 상한으로 상한 경계도 자주 밟게 한다 */
    uint32_t seed = 12345;
    int iter;

    for (iter = 0; iter < 2000; iter++) {
        const char *base = bases[iter % 3];
        size_t n = strlen(base);
        char s[256];
        int k, edits = 1 + (int)(lcg(&seed) % 3);
        buf_t whole;

        memcpy(s, base, n + 1);
        for (k = 0; k < edits; k++)
            s[lcg(&seed) % n] = alphabet[lcg(&seed) % (sizeof alphabet - 1)];
        if (lcg(&seed) % 4 == 0)
            n = lcg(&seed) % n; /* 가끔 잘라서 미완성 입력도 */
        parse_all_modes("fuzz", s, n, (iter & 1) ? &lim : NULL, &whole);
        buf_free(&whole);
    }
}

int main(void)
{
    RUN_TEST(test_ok_cases);
    RUN_TEST(test_absolute_form);
    RUN_TEST(test_header_lookup);
    RUN_TEST(test_method_name);
    RUN_TEST(test_error_table);
    RUN_TEST(test_limits_boundaries);
    RUN_TEST(test_default_limits);
    RUN_TEST(test_early_rejection);
    RUN_TEST(test_keep_alive);
    RUN_TEST(test_pipelining);
    RUN_TEST(test_error_is_sticky);
    RUN_TEST(test_need_more_contract);
    RUN_TEST(test_max_headers_clamped);
    RUN_TEST(test_fuzz_split_invariance);
    TEST_END();
}
