/*
 * test_http_response.c — 상태줄/헤더 형식, 날짜, HTML 상태 페이지.
 */
#include "test.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_response.h"
#include "netio.h"

static void test_reason_phrases(void)
{
    static const struct {
        int code;
        const char *reason;
    } cases[] = {
        {200, "OK"}, {301, "Moved Permanently"}, {400, "Bad Request"}, {403, "Forbidden"},
        {404, "Not Found"}, {405, "Method Not Allowed"}, {411, "Length Required"},
        {413, "Content Too Large"}, {414, "URI Too Long"}, {431, "Request Header Fields Too Large"},
        {500, "Internal Server Error"}, {501, "Not Implemented"}, {502, "Bad Gateway"},
        {503, "Service Unavailable"}, {504, "Gateway Timeout"}, {505, "HTTP Version Not Supported"},
        {999, "Unknown"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK_EQ_STR(http_status_reason(cases[i].code), cases[i].reason);
}

static void test_http_date(void)
{
    char d[HTTP_DATE_LEN];

    http_date(d, 784111777); /* RFC 9110 의 예시 시각 */
    CHECK_EQ_STR(d, "Sun, 06 Nov 1994 08:49:37 GMT");
    http_date(d, 0);
    CHECK_EQ_STR(d, "Thu, 01 Jan 1970 00:00:00 GMT");
}

static void test_head_format(void)
{
    buf_t b;

    buf_init(&b, 0, 4096);
    CHECK_EQ_INT(http_response_head(&b, 404, "text/plain", 12, 0, "X-A: 1\r\n"), BUF_OK);
    CHECK(strncmp(b.data, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(b.data, "\r\nDate: ") != NULL);
    CHECK(strstr(b.data, " GMT\r\n") != NULL);
    CHECK(strstr(b.data, "\r\nServer: " HTTP_SERVER_NAME "\r\n") != NULL);
    CHECK(strstr(b.data, "\r\nContent-Type: text/plain\r\n") != NULL);
    CHECK(strstr(b.data, "\r\nContent-Length: 12\r\n") != NULL);
    CHECK(strstr(b.data, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(b.data, "\r\nX-A: 1\r\n") != NULL);
    CHECK_EQ_STR(b.data + b.len - 4, "\r\n\r\n"); /* 헤더 끝 */
    CHECK(strstr(b.data, "\r\n\r\n") == b.data + b.len - 4); /* 빈 줄은 정확히 한 번 */

    buf_reset(&b);
    CHECK_EQ_INT(http_response_head(&b, 200, NULL, 0, 1, NULL), BUF_OK);
    CHECK(strstr(b.data, "Content-Type") == NULL);
    CHECK(strstr(b.data, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(b.data, "\r\nContent-Length: 0\r\n") != NULL);
    buf_free(&b);
}

/* Content-Length 가 실제 본문 길이와 일치해야 keep-alive 에서 다음 응답을 읽을 수 있다. */
static void test_status_page_length(void)
{
    buf_t b;
    const char *body;
    const char *cl;
    long n;

    buf_init(&b, 0, 8192);
    CHECK_EQ_INT(http_response_build_status_page(&b, 404, 1, 0, NULL), BUF_OK);
    body = strstr(b.data, "\r\n\r\n");
    REQUIRE(body != NULL);
    body += 4;
    cl = strstr(b.data, "Content-Length: ");
    REQUIRE(cl != NULL);
    n = strtol(cl + 16, NULL, 10);
    CHECK_EQ_SIZE((size_t)n, strlen(body));
    CHECK(strstr(body, "404 Not Found") != NULL);
    CHECK(strstr(b.data, "Content-Type: text/html; charset=utf-8") != NULL);

    /* HEAD: 같은 Content-Length, 본문 없음 */
    buf_reset(&b);
    CHECK_EQ_INT(http_response_build_status_page(&b, 404, 1, 1, NULL), BUF_OK);
    CHECK_EQ_STR(b.data + b.len - 4, "\r\n\r\n");
    cl = strstr(b.data, "Content-Length: ");
    REQUIRE(cl != NULL);
    CHECK_EQ_INT(strtol(cl + 16, NULL, 10), n);
    buf_free(&b);
}

static void test_status_page_send(void)
{
    int sv[2];
    char buf[4096];
    size_t sent = 0;
    ssize_t got;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK_EQ_INT(http_response_status_page(&(conn_t){sv[0], NULL, 0}, 405, 0, 0, "Allow: GET, HEAD\r\n", &sent), 0);
    got = recv_some(sv[1], buf, sizeof buf - 1, 1000);
    REQUIRE(got > 0);
    buf[got] = '\0';
    CHECK_EQ_SIZE((size_t)got, sent);
    CHECK(strncmp(buf, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(strstr(buf, "\r\nAllow: GET, HEAD\r\n") != NULL);
    close(sv[0]);
    close(sv[1]);
}

static void test_backend_name(void)
{
    buf_t b;

    buf_init(&b, 0, 4096);
    http_response_head(&b, 200, NULL, 0, 1, NULL);
    CHECK(strstr(b.data, "X-Backend") == NULL); /* 기본은 없음 */
    http_response_set_backend_name("A");
    buf_reset(&b);
    http_response_head(&b, 200, NULL, 0, 1, NULL);
    CHECK(strstr(b.data, "\r\nX-Backend: A\r\n") != NULL);
    buf_reset(&b);
    http_response_build_status_page(&b, 503, 0, 0, NULL); /* 오류 페이지에도 */
    CHECK(strstr(b.data, "\r\nX-Backend: A\r\n") != NULL);
    http_response_set_backend_name(NULL);
    buf_free(&b);
}

int main(void)
{
    RUN_TEST(test_reason_phrases);
    RUN_TEST(test_http_date);
    RUN_TEST(test_head_format);
    RUN_TEST(test_status_page_length);
    RUN_TEST(test_status_page_send);
    RUN_TEST(test_backend_name);
    TEST_END();
}
