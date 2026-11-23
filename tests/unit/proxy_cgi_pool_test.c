#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#include "cgi.h"
#include "proxy.h"
#include "test.h"
#include "threadpool.h"

static int parse(const char *raw, http_request_t *req) {
    size_t used;
    int status;
    return http_parse_request(raw, strlen(raw), 1 << 20, req, &used, &status);
}

static void test_hop_headers(void) {
    CHECK(proxy_is_hop_header("connection", NULL));
    CHECK(proxy_is_hop_header("Keep-Alive", NULL));
    CHECK(proxy_is_hop_header("transfer-encoding", NULL));
    CHECK(proxy_is_hop_header("Upgrade", NULL));
    CHECK(proxy_is_hop_header("X-Secret-Hop", "keep-alive, X-Secret-Hop"));
    CHECK(!proxy_is_hop_header("Accept", "keep-alive"));
    CHECK(!proxy_is_hop_header("Content-Type", NULL));
}

static void test_build_request(void) {
    http_request_t req;
    const char *raw = "POST /api/x?q=1 HTTP/1.1\r\nHost: front.test\r\nConnection: keep-alive, X-Hop\r\n"
                      "X-Hop: drop-me\r\nKeep-Alive: timeout=5\r\nAccept: */*\r\n"
                      "X-Forwarded-For: 10.0.0.1\r\nVia: 1.1 edge\r\nExpect: 100-continue\r\n"
                      "Transfer-Encoding: chunked\r\nProxy-Authorization: secret\r\n\r\n"
                      "3\r\nabc\r\n0\r\n\r\n";
    CHECK_INT(parse(raw, &req), HTTP_PARSE_OK);
    buf_t out;
    buf_init(&out);
    CHECK_INT(proxy_build_request(&out, &req, "/x?q=1", "front.test", "192.168.1.9"), 0);
    const char *s = out.data;
    CHECK(strncmp(s, "POST /x?q=1 HTTP/1.1\r\nHost: front.test\r\n", 40) == 0);
    CHECK(strstr(s, "Accept: */*\r\n") != NULL);
    CHECK(strstr(s, "X-Forwarded-For: 10.0.0.1, 192.168.1.9\r\n") != NULL);
    CHECK(strstr(s, "X-Forwarded-Host: front.test\r\n") != NULL);
    CHECK(strstr(s, "X-Forwarded-Proto: http\r\n") != NULL);
    CHECK(strstr(s, "Via: 1.1 edge, 1.1 cwebserv\r\n") != NULL);
    CHECK(strstr(s, "Content-Length: 3\r\n") != NULL); /* chunked body re-framed */
    CHECK(strstr(s, "Connection: close\r\n\r\nabc") != NULL);
    CHECK(strstr(s, "X-Hop") == NULL);
    CHECK(strstr(s, "Keep-Alive") == NULL);
    CHECK(strstr(s, "Transfer-Encoding") == NULL);
    CHECK(strstr(s, "Expect") == NULL);
    CHECK(strstr(s, "Proxy-Authorization") == NULL);
    buf_free(&out);
    http_request_free(&req);

    const char *get10 = "GET /p HTTP/1.0\r\n\r\n";
    CHECK_INT(parse(get10, &req), HTTP_PARSE_OK);
    buf_init(&out);
    proxy_build_request(&out, &req, "/p", "b:1", "1.2.3.4");
    CHECK(strncmp(out.data, "GET /p HTTP/1.0\r\n", 17) == 0); /* 1.0 client: no chunked back */
    CHECK(strstr(out.data, "Content-Length") == NULL);
    CHECK(strstr(out.data, "X-Forwarded-Host") == NULL);
    buf_free(&out);
    http_request_free(&req);
}

static void test_response_lines(void) {
    const char *raw = "HTTP/1.0 200 Fine\r\nConnection: keep-alive, X-Internal\r\nKeep-Alive: 5\r\n"
                      "X-Internal: 1\r\nTransfer-Encoding: chunked\r\nSet-Cookie: a=b\r\n\r\n";
    http_response_head_t res;
    CHECK_INT(http_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_OK);
    buf_t out;
    buf_init(&out);
    proxy_response_head_lines(&out, &res);
    CHECK(strncmp(out.data, "HTTP/1.1 200 Fine\r\n", 19) == 0);
    CHECK(strstr(out.data, "Transfer-Encoding: chunked\r\n") != NULL); /* relayed raw */
    CHECK(strstr(out.data, "Set-Cookie: a=b\r\n") != NULL);
    CHECK(strstr(out.data, "Connection") == NULL);
    CHECK(strstr(out.data, "Keep-Alive") == NULL);
    CHECK(strstr(out.data, "X-Internal") == NULL);
    CHECK(strstr(out.data, "\r\n\r\n") == NULL); /* caller adds Connection + blank line */
    buf_free(&out);
    http_response_head_free(&res);
}

void suite_proxy(void) {
    test_hop_headers();
    test_build_request();
    test_response_lines();
}

static void test_cgi_output(void) {
    int status;
    char ctype[64];
    buf_t extra;
    size_t off;
    const char *a = "Content-Type: text/html\r\nX-A: 1\r\n\r\n<p>hi</p>";
    buf_init(&extra);
    CHECK_INT(cgi_parse_output(a, strlen(a), &status, ctype, sizeof(ctype), &extra, &off), 0);
    CHECK_INT(status, 200);
    CHECK_STR(ctype, "text/html");
    CHECK_STR(extra.data, "X-A: 1\r\n");
    CHECK_STR(a + off, "<p>hi</p>");
    buf_free(&extra);

    const char *b = "Status: 404 Nope\nContent-Length: 999\nConnection: keep-alive\n\nmissing";
    buf_init(&extra);
    CHECK_INT(cgi_parse_output(b, strlen(b), &status, ctype, sizeof(ctype), &extra, &off), 0);
    CHECK_INT(status, 404);
    CHECK_STR(ctype, "text/plain; charset=utf-8"); /* default */
    CHECK(extra.len == 0);                        /* framing headers are the server's */
    CHECK_STR(b + off, "missing");
    buf_free(&extra);

    const char *c = "Location: /elsewhere\r\n\r\n";
    buf_init(&extra);
    CHECK_INT(cgi_parse_output(c, strlen(c), &status, ctype, sizeof(ctype), &extra, &off), 0);
    CHECK_INT(status, 302);
    CHECK_STR(extra.data, "Location: /elsewhere\r\n");
    buf_free(&extra);

    const char *bad[] = {"no header block", "Bad Header\r\n\r\n", "Status: abc\r\n\r\n",
                         "Status: 99\r\n\r\n", ": empty\r\n\r\n", "X: a\001b\r\n\r\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        buf_init(&extra);
        CHECK_INT(cgi_parse_output(bad[i], strlen(bad[i]), &status, ctype, sizeof(ctype), &extra, &off), -1);
        buf_free(&extra);
    }
}

static int env_has(char **env, const char *kv) {
    for (char **e = env; *e; e++)
        if (strcmp(*e, kv) == 0)
            return 1;
    return 0;
}

static void test_cgi_env(void) {
    static config_t cfg;
    config_defaults(&cfg);
    server_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.cfg = &cfg;
    conn_t conn;
    memset(&conn, 0, sizeof(conn));
    strcpy(conn.peer, "10.1.2.3");
    http_request_t req;
    const char *raw = "POST /cgi-bin/s.sh/x/y?a=1 HTTP/1.1\r\nHost: h.test\r\nContent-Type: text/plain\r\n"
                      "User-Agent: unit\r\nProxy: http://evil:1\r\nX-Custom-Name: v\r\n"
                      "Content-Length: 2\r\n\r\nok";
    CHECK_INT(parse(raw, &req), HTTP_PARSE_OK);
    req_ctx_t ctx = {.srv = &srv, .conn = &conn, .req = &req};
    char **env = cgi_build_env(&ctx, "/cgi-bin/s.sh", "/x/y");
    CHECK(env != NULL);
    CHECK(env_has(env, "REQUEST_METHOD=POST"));
    CHECK(env_has(env, "SCRIPT_NAME=/cgi-bin/s.sh"));
    CHECK(env_has(env, "PATH_INFO=/x/y"));
    CHECK(env_has(env, "QUERY_STRING=a=1"));
    CHECK(env_has(env, "CONTENT_LENGTH=2"));
    CHECK(env_has(env, "CONTENT_TYPE=text/plain"));
    CHECK(env_has(env, "REMOTE_ADDR=10.1.2.3"));
    CHECK(env_has(env, "SERVER_PROTOCOL=HTTP/1.1"));
    CHECK(env_has(env, "HTTP_USER_AGENT=unit"));
    CHECK(env_has(env, "HTTP_X_CUSTOM_NAME=v"));
    CHECK(!env_has(env, "HTTP_PROXY=http://evil:1")); /* httpoxy */
    CHECK(!env_has(env, "HTTP_CONTENT_TYPE=text/plain"));
    cgi_free_env(env);
    http_request_free(&req);
}

void suite_cgi(void) {
    test_cgi_output();
    test_cgi_env();
}

static _Atomic int counter;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;

static void bump(void *arg) {
    (void)arg;
    atomic_fetch_add(&counter, 1);
}

static void wait_gate(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    atomic_fetch_add(&counter, 1);
}

void suite_threadpool(void) {
    threadpool_t *p = threadpool_create(4, 2000);
    CHECK(p != NULL);
    CHECK_INT(threadpool_size(p), 4);
    for (int i = 0; i < 1000; i++)
        CHECK_INT(threadpool_submit(p, bump, NULL), 0);
    threadpool_destroy(p); /* drains the queue before joining */
    CHECK_INT(atomic_load(&counter), 1000);

    atomic_store(&counter, 0);
    pthread_mutex_lock(&gate);
    p = threadpool_create(1, 2);
    CHECK_INT(threadpool_submit(p, wait_gate, NULL), 0);
    while (threadpool_pending(p) != 0) /* the single worker picked it up and is blocked */
        usleep(1000);
    CHECK_INT(threadpool_submit(p, wait_gate, NULL), 0);
    CHECK_INT(threadpool_submit(p, wait_gate, NULL), 0);
    CHECK_INT(threadpool_submit(p, wait_gate, NULL), -1); /* bounded queue is full */
    pthread_mutex_unlock(&gate);
    threadpool_destroy(p);
    CHECK_INT(atomic_load(&counter), 3);
}
