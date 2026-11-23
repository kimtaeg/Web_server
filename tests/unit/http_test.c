#include <stdlib.h>

#include "http.h"
#include "test.h"

static int status_of(const char *raw) {
    http_request_t req;
    size_t used;
    int status;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), 1024, &req, &used, &status);
    if (r == HTTP_PARSE_OK) {
        http_request_free(&req);
        return 200;
    }
    return r == HTTP_PARSE_INCOMPLETE ? 0 : status;
}

static void test_simple_get(void) {
    const char *raw = "GET /a/b.html?x=1&y=%20 HTTP/1.1\r\nHost: example.com\r\n"
                      "User-Agent: t\r\nX-Pad:   spaced value  \r\n\r\n";
    http_request_t req;
    size_t used;
    int status;
    CHECK_INT(http_parse_request(raw, strlen(raw), 1024, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_INT(used, strlen(raw));
    CHECK_INT(req.method, HTTP_GET);
    CHECK_STR(req.method_str, "GET");
    CHECK_STR(req.target, "/a/b.html?x=1&y=%20");
    CHECK_STR(req.path, "/a/b.html");
    CHECK_STR(req.query, "x=1&y=%20");
    CHECK_INT(req.version_minor, 1);
    CHECK_INT(req.form, TARGET_ORIGIN);
    CHECK_INT(req.keep_alive, 1);
    CHECK_INT(req.header_count, 3);
    CHECK_STR(http_request_header(&req, "host"), "example.com");
    CHECK_STR(http_request_header(&req, "X-PAD"), "spaced value");
    CHECK(http_request_header(&req, "missing") == NULL);
    CHECK_INT(req.content_length, -1);
    http_request_free(&req);
}

static void test_incremental(void) {
    const char *raw = "POST /f HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
    size_t len = strlen(raw);
    for (size_t cut = 0; cut < len; cut++) {
        http_request_t req;
        size_t used;
        int status = -1;
        http_parse_result_t r = http_parse_request(raw, cut, 1024, &req, &used, &status);
        if (r != HTTP_PARSE_INCOMPLETE) {
            CHECK_INT(r, HTTP_PARSE_INCOMPLETE);
            if (r == HTTP_PARSE_OK)
                http_request_free(&req);
            return;
        }
    }
    http_request_t req;
    size_t used;
    int status;
    CHECK_INT(http_parse_request(raw, len, 1024, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_INT(req.body_len, 5);
    CHECK(memcmp(req.body, "hello", 5) == 0);
    http_request_free(&req);
}

static void test_pipelined(void) {
    const char *raw = "GET /1 HTTP/1.1\r\nHost: h\r\n\r\nGET /2 HTTP/1.1\r\nHost: h\r\n\r\n";
    http_request_t req;
    size_t used;
    int status;
    CHECK_INT(http_parse_request(raw, strlen(raw), 1024, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_STR(req.path, "/1");
    http_request_free(&req);
    CHECK_INT(http_parse_request(raw + used, strlen(raw + used), 1024, &req, &used, &status),
              HTTP_PARSE_OK);
    CHECK_STR(req.path, "/2");
    http_request_free(&req);
}

static void test_keep_alive_rules(void) {
    http_request_t req;
    size_t used;
    int status;
    const char *a = "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
    http_parse_request(a, strlen(a), 0, &req, &used, &status);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);
    const char *b = "GET / HTTP/1.0\r\n\r\n";
    http_parse_request(b, strlen(b), 0, &req, &used, &status);
    CHECK_INT(req.keep_alive, 0);
    CHECK_INT(req.version_minor, 0);
    http_request_free(&req);
    const char *c = "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n";
    http_parse_request(c, strlen(c), 0, &req, &used, &status);
    CHECK_INT(req.keep_alive, 1);
    http_request_free(&req);
    const char *d = "GET / HTTP/1.1\r\nHost: h\r\nConnection: foo, close\r\n\r\n";
    http_parse_request(d, strlen(d), 0, &req, &used, &status);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);
}

static void test_errors(void) {
    CHECK_INT(status_of("GET / HTTP/1.1\r\n\r\n"), 400);                     /* no Host */
    CHECK_INT(status_of("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), 400);
    CHECK_INT(status_of("GET / HTTP/2.0\r\nHost: a\r\n\r\n"), 505);
    CHECK_INT(status_of("GET / HTTP/1.1x\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET / http/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET /\r\n\r\n"), 400);
    CHECK_INT(status_of("GET  / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("FOO / HTTP/1.1\r\nHost: a\r\n\r\n"), 501);
    CHECK_INT(status_of("get / HTTP/1.1\r\nHost: a\r\n\r\n"), 501);         /* case-sensitive */
    CHECK_INT(status_of("G@T / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET / HTTP/1.1\r\nHost : a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n"), 400);
    CHECK_INT(status_of("GET / HTTP/1.1\r\nHost: a\r\nnocolon\r\n\r\n"), 400);
    CHECK_INT(status_of("GET / HTTP/1.1\r\nHost: a\x01\r\n\r\n"), 400);
    CHECK_INT(status_of("GET /a%00 HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET /a%4 HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET /../etc/passwd HTTP/1.1\r\nHost: a\r\n\r\n"), 403);
    CHECK_INT(status_of("GET /a/%2e%2e/%2e%2e/x HTTP/1.1\r\nHost: a\r\n\r\n"), 403);
    CHECK_INT(status_of("GET x HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("GET * HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(status_of("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n"), 200);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n"), 400);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx"), 200);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx"), 400);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2048\r\n\r\n"), 413);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999\r\n\r\n"), 413);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n"), 400);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n"), 501);
    CHECK_INT(status_of("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"), 400);
    CHECK_INT(status_of("POST / HTTP/1.1\r\nHost: a\r\nExpect: nope\r\n\r\n"), 417);
}

static void test_limits(void) {
    size_t n = HTTP_MAX_URI + 100;
    char *raw = malloc(n + 64);
    memcpy(raw, "GET /", 5);
    memset(raw + 5, 'a', n);
    strcpy(raw + 5 + n, " HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_INT(status_of(raw), 414);
    raw[5 + n] = '\0'; /* still receiving the request line */
    CHECK_INT(status_of(raw), 414);
    free(raw);

    size_t hn = HTTP_MAX_URI + HTTP_MAX_HEADER_BYTES + 10;
    char *big = malloc(hn + 64);
    strcpy(big, "GET / HTTP/1.1\r\nX: ");
    memset(big + 19, 'b', hn);
    big[19 + hn] = '\0';
    CHECK_INT(status_of(big), 431);
    free(big);

    char many[8192] = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < HTTP_MAX_HEADERS + 1; i++)
        sprintf(many + strlen(many), "X-%d: v\r\n", i);
    strcat(many, "\r\n");
    CHECK_INT(status_of(many), 431);
}

static void test_chunked(void) {
    const char *raw = "POST /u HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "4;name=v\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n"
                      "Trailer: x\r\n\r\nNEXT";
    http_request_t req;
    size_t used;
    int status;
    CHECK_INT(http_parse_request(raw, strlen(raw), 1024, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_INT(req.chunked, 1);
    CHECK_INT(req.body_len, 23);
    CHECK(memcmp(req.body, "Wikipedia in\r\n\r\nchunks.", 23) == 0);
    CHECK_STR(raw + used, "NEXT");
    http_request_free(&req);

    const char *part = "POST /u HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWi";
    CHECK_INT(status_of(part), 0);
    CHECK_INT(status_of("POST /u HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\nxyz\r\n"), 400);
    CHECK_INT(status_of("POST /u HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n800\r\n"), 413);
}

static void test_expect_continue(void) {
    const char *raw = "POST /u HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";
    http_request_t req;
    size_t used;
    int status = 0;
    CHECK_INT(http_parse_request(raw, strlen(raw), 1024, &req, &used, &status),
              HTTP_PARSE_INCOMPLETE);
    CHECK_INT(status, 100);
}

static void test_targets(void) {
    http_request_t req;
    size_t used;
    int status;
    const char *abs = "GET http://Example.com:8081/p%20q?z=1 HTTP/1.1\r\nHost: ignored\r\n\r\n";
    CHECK_INT(http_parse_request(abs, strlen(abs), 0, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_INT(req.form, TARGET_ABSOLUTE);
    CHECK_STR(req.host, "Example.com");
    CHECK_INT(req.port, 8081);
    CHECK_STR(req.path, "/p q");
    CHECK_STR(req.query, "z=1");
    http_request_free(&req);

    const char *noport = "GET http://h.test HTTP/1.1\r\nHost: h.test\r\n\r\n";
    CHECK_INT(http_parse_request(noport, strlen(noport), 0, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_INT(req.port, 80);
    CHECK_STR(req.path, "/");
    http_request_free(&req);

    const char *conn = "CONNECT secure.test:443 HTTP/1.1\r\nHost: secure.test:443\r\n\r\n";
    CHECK_INT(http_parse_request(conn, strlen(conn), 0, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_INT(req.form, TARGET_AUTHORITY);
    CHECK_STR(req.host, "secure.test");
    CHECK_INT(req.port, 443);
    http_request_free(&req);

    const char *v6 = "CONNECT [::1]:8443 HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK_INT(http_parse_request(v6, strlen(v6), 0, &req, &used, &status), HTTP_PARSE_OK);
    CHECK_STR(req.host, "::1");
    CHECK_INT(req.port, 8443);
    http_request_free(&req);

    CHECK_INT(status_of("CONNECT nohost HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_INT(status_of("CONNECT h:99999 HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_INT(status_of("GET http://user@h/ HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_INT(status_of("GET ftp://h/ HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
}

static void test_normalize(void) {
    struct {
        const char *in, *out;
    } cases[] = {
        {"/", "/"},           {"/a/b", "/a/b"},       {"//a///b", "/a/b"},
        {"/a/./b", "/a/b"},   {"/a/b/../c", "/a/c"},  {"/a/b/..", "/a/"},
        {"/a/.", "/a/"},      {"/a/", "/a/"},         {"/a/b/../../c", "/c"},
        {"/./", "/"},         {"/a/..", "/"},         {"/.hidden", "/.hidden"},
        {"/a..b/", "/a..b/"}, {"/...", "/..."},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[64];
        strcpy(buf, cases[i].in);
        CHECK_INT(http_normalize_path(buf), 0);
        CHECK_STR(buf, cases[i].out);
    }
    char up[] = "/a/../../b";
    CHECK_INT(http_normalize_path(up), -1);
    char up2[] = "/..";
    CHECK_INT(http_normalize_path(up2), -1);
    char rel[] = "a/b";
    CHECK_INT(http_normalize_path(rel), -1);
}

static void test_url_codec(void) {
    char out[64];
    CHECK_INT(http_url_decode("/a%20b%2Fc", 10, out, sizeof(out)), 6);
    CHECK_STR(out, "/a b/c");
    CHECK_INT(http_url_decode("a+b", 3, out, sizeof(out)), 3);
    CHECK_STR(out, "a+b"); /* '+' means space only in form bodies, not paths */
    CHECK_INT(http_url_decode("%", 1, out, sizeof(out)), -1);
    CHECK_INT(http_url_decode("%g0", 3, out, sizeof(out)), -1);
    CHECK_INT(http_url_decode("%00", 3, out, sizeof(out)), -1);
    CHECK_INT(http_url_decode("abcdef", 6, out, 4), -1);

    buf_t b;
    buf_init(&b);
    http_encode_path(&b, "/a b/ü?#%");
    CHECK_STR(b.data, "/a%20b/%C3%BC%3F%23%25");
    buf_free(&b);
}

static void test_response_head(void) {
    const char *raw = "HTTP/1.1 404 Not Found\r\nContent-Length: 12\r\nConnection: close\r\n"
                      "X-A: b\r\n\r\nbody";
    http_response_head_t res;
    CHECK_INT(http_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_OK);
    CHECK_INT(res.status, 404);
    CHECK_STR(res.reason, "Not Found");
    CHECK_INT(res.content_length, 12);
    CHECK_INT(res.connection_close, 1);
    CHECK_INT(res.head_len, strlen(raw) - 4);
    http_response_head_free(&res);

    const char *chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n";
    CHECK_INT(http_parse_response_head(chunked, strlen(chunked), &res), HTTP_PARSE_OK);
    CHECK_INT(res.chunked, 1);
    CHECK_INT(res.content_length, -1);
    http_response_head_free(&res);

    const char *noreason = "HTTP/1.0 204\r\n\r\n";
    CHECK_INT(http_parse_response_head(noreason, strlen(noreason), &res), HTTP_PARSE_OK);
    CHECK_INT(res.status, 204);
    CHECK_INT(res.version_minor, 0);
    http_response_head_free(&res);

    CHECK_INT(http_parse_response_head("HTTP/1.1 200 OK\r\n", 17, &res), HTTP_PARSE_INCOMPLETE);
    CHECK_INT(http_parse_response_head("SMTP ready\r\n\r\n", 14, &res), HTTP_PARSE_ERROR);
    CHECK_INT(http_parse_response_head("HTTP/1.1 2x0 OK\r\n\r\n", 19, &res), HTTP_PARSE_ERROR);
}

static void test_chunk_scanner(void) {
    const char *body = "5\r\nhello\r\n1;x=y\r\n!\r\n0\r\nT: v\r\n\r\nEXTRA";
    size_t len = strlen(body);
    /* Feed one byte at a time: the scanner must find exactly the same end. */
    chunk_scanner_t s;
    chunk_scanner_init(&s);
    char out[32];
    size_t out_len = 0, pos = 0;
    int done = 0;
    while (!done && pos < len) {
        long used = chunk_scanner_feed(&s, body + pos, 1, out, &out_len, &done);
        CHECK(used == 1);
        pos += (size_t)used;
    }
    CHECK_INT(done, 1);
    CHECK_INT(pos, len - 5);
    CHECK_INT(out_len, 6);
    CHECK(memcmp(out, "hello!", 6) == 0);

    chunk_scanner_init(&s);
    out_len = 0;
    CHECK_INT(chunk_scanner_feed(&s, "5\r\nhelloXX", 10, NULL, &out_len, &done), -1);
    chunk_scanner_init(&s);
    CHECK_INT(chunk_scanner_feed(&s, "1234567890ABCDEF0\r\n", 19, NULL, &out_len, &done), -1);
}

static void test_tokens_and_misc(void) {
    CHECK(http_header_has_token("keep-alive, Upgrade", "upgrade"));
    CHECK(http_header_has_token(" close ", "close"));
    CHECK(!http_header_has_token("closed", "close"));
    CHECK(!http_header_has_token(NULL, "close"));
    CHECK_INT(http_method_from_str("DELETE", 6), HTTP_DELETE);
    CHECK_INT(http_method_from_str("GETX", 4), HTTP_UNKNOWN);
    CHECK_STR(http_reason(404), "Not Found");
    CHECK_STR(http_reason(799), "Unknown");
    char date[64];
    http_format_date(0, date, sizeof(date));
    CHECK_STR(date, "Thu, 01 Jan 1970 00:00:00 GMT");
}

void suite_http(void) {
    test_simple_get();
    test_incremental();
    test_pipelined();
    test_keep_alive_rules();
    test_errors();
    test_limits();
    test_chunked();
    test_expect_continue();
    test_targets();
    test_normalize();
    test_url_codec();
    test_response_head();
    test_chunk_scanner();
    test_tokens_and_misc();
}
