/*
 * test_buf.c — buf 모듈 단위 테스트.
 */
#include "test.h"

#include <stdint.h>

#include "buf.h"

static void test_init_free(void)
{
    buf_t b;

    CHECK_EQ_INT(buf_init(&b, 16, 1024), BUF_OK);
    CHECK(b.data != NULL);
    CHECK_EQ_SIZE(b.len, 0);
    CHECK(b.cap >= 17); /* NUL 자리 포함 */
    CHECK_EQ_INT(b.data[0], '\0');
    buf_free(&b);
    CHECK(b.data == NULL);
    CHECK_EQ_SIZE(b.cap, 0);
    buf_free(&b); /* 두 번 free 해도 안전 */
}

static void test_init_lazy_and_invalid(void)
{
    buf_t b;

    CHECK_EQ_INT(buf_init(&b, 0, 100), BUF_OK);
    CHECK(b.data == NULL);
    CHECK_EQ_INT(buf_append(&b, "hi", 2), BUF_OK);
    CHECK_EQ_STR(b.data, "hi");
    buf_free(&b);

    CHECK_EQ_INT(buf_init(&b, 10, 0), BUF_ERR_LIMIT); /* max 0 은 무의미 */
    buf_free(&b);

    /* init_cap > max 는 max 로 잘린다 */
    CHECK_EQ_INT(buf_init(&b, 1000, 8), BUF_OK);
    CHECK_EQ_SIZE(b.cap, 9);
    buf_free(&b);
}

static void test_append_basic(void)
{
    buf_t b;

    buf_init(&b, 0, 1024);
    CHECK_EQ_INT(buf_append(&b, "GET ", 4), BUF_OK);
    CHECK_EQ_INT(buf_append(&b, "/ HTTP/1.1", 10), BUF_OK);
    CHECK_EQ_SIZE(b.len, 14);
    CHECK_EQ_STR(b.data, "GET / HTTP/1.1");
    CHECK_EQ_INT(buf_append(&b, "x", 0), BUF_OK); /* 0바이트는 no-op */
    CHECK_EQ_SIZE(b.len, 14);
    buf_free(&b);
}

static void test_append_grows(void)
{
    buf_t b;
    size_t i;
    int ok = 1;

    buf_init(&b, 1, 1 << 20);
    for (i = 0; i < 10000; i++) {
        char c = (char)('a' + (int)(i % 26));
        if (buf_append(&b, &c, 1) != BUF_OK)
            ok = 0;
    }
    CHECK(ok);
    REQUIRE(b.len == 10000);
    CHECK(b.cap > b.len);
    CHECK_EQ_INT(b.data[0], 'a');
    CHECK_EQ_INT(b.data[9999], 'a' + (9999 % 26));
    CHECK_EQ_INT(b.data[10000], '\0');
    buf_free(&b);
}

static void test_append_limit(void)
{
    buf_t b;

    buf_init(&b, 0, 8);
    CHECK_EQ_INT(buf_append(&b, "12345678", 8), BUF_OK); /* 정확히 max 는 허용 */
    CHECK_EQ_SIZE(b.len, 8);
    CHECK_EQ_INT(buf_append(&b, "9", 1), BUF_ERR_LIMIT);
    CHECK_EQ_SIZE(b.len, 8); /* 실패 시 내용 불변 */
    CHECK_EQ_STR(b.data, "12345678");

    buf_reset(&b);
    CHECK_EQ_INT(buf_append(&b, "123456789", 9), BUF_ERR_LIMIT);
    CHECK_EQ_SIZE(b.len, 0);

    /* len + n 이 넘치는 크기도 overflow 없이 LIMIT 으로 거부 */
    CHECK_EQ_INT(buf_append(&b, "x", 1), BUF_OK);
    CHECK_EQ_INT(buf_append(&b, "x", SIZE_MAX), BUF_ERR_LIMIT);
    CHECK_EQ_SIZE(b.len, 1);
    buf_free(&b);
}

static void test_appendf(void)
{
    buf_t b;

    buf_init(&b, 0, 64);
    CHECK_EQ_INT(buf_appendf(&b, "HTTP/1.1 %d %s\r\n", 404, "Not Found"), BUF_OK);
    CHECK_EQ_STR(b.data, "HTTP/1.1 404 Not Found\r\n");
    CHECK_EQ_INT(buf_appendf(&b, "Content-Length: %zu\r\n", (size_t)12), BUF_OK);
    CHECK_EQ_STR(b.data, "HTTP/1.1 404 Not Found\r\nContent-Length: 12\r\n");
    CHECK_EQ_INT(buf_appendf(&b, "%s", ""), BUF_OK);

    /* 상한을 넘는 출력은 통째로 거부, 내용 불변 */
    {
        size_t before = b.len;
        CHECK_EQ_INT(buf_appendf(&b, "%0100d", 1), BUF_ERR_LIMIT);
        CHECK_EQ_SIZE(b.len, before);
        CHECK_EQ_INT(b.data[b.len], '\0');
    }
    buf_free(&b);
}

static void test_consume(void)
{
    buf_t b;

    buf_init(&b, 0, 64);
    buf_append(&b, "abcdef", 6);
    buf_consume(&b, 2);
    CHECK_EQ_SIZE(b.len, 4);
    CHECK_EQ_STR(b.data, "cdef");
    buf_consume(&b, 0);
    CHECK_EQ_STR(b.data, "cdef");
    buf_consume(&b, 4);
    CHECK_EQ_SIZE(b.len, 0);
    CHECK_EQ_STR(b.data, "");

    buf_append(&b, "xyz", 3);
    buf_consume(&b, 100); /* len 보다 많이 → 비우기 */
    CHECK_EQ_SIZE(b.len, 0);
    CHECK_EQ_STR(b.data, "");
    buf_free(&b);

    buf_init(&b, 0, 64);
    buf_consume(&b, 3); /* 미할당 상태에서도 안전 */
    CHECK_EQ_SIZE(b.len, 0);
    buf_free(&b);
}

/* 파이프라이닝: 요청 하나를 소비하면 다음 요청이 앞으로 당겨진다. */
static void test_consume_pipelined(void)
{
    buf_t b;
    const char *two = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t end;

    buf_init(&b, 0, 1024);
    buf_append(&b, two, strlen(two));
    end = buf_find(&b, 0, "\r\n\r\n", 4);
    REQUIRE(end > 0);
    buf_consume(&b, (size_t)end + 4);
    CHECK(strncmp(b.data, "GET /b ", 7) == 0);
    end = buf_find(&b, 0, "\r\n\r\n", 4);
    REQUIRE(end > 0);
    buf_consume(&b, (size_t)end + 4);
    CHECK_EQ_SIZE(b.len, 0);
    buf_free(&b);
}

static void test_find_basic(void)
{
    buf_t b;
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\nBODY";

    buf_init(&b, 0, 1024);
    CHECK_EQ_INT(buf_find(&b, 0, "\r\n\r\n", 4), -1); /* 빈 버퍼 */
    buf_append(&b, req, strlen(req));
    CHECK_EQ_INT(buf_find(&b, 0, "\r\n\r\n", 4), 23);
    CHECK_EQ_INT(buf_find(&b, 0, "\r\n", 2), 14);
    CHECK_EQ_INT(buf_find(&b, 15, "\r\n", 2), 23); /* from 이후만 */
    CHECK_EQ_INT(buf_find(&b, 0, "GET", 3), 0);    /* 맨 앞 */
    CHECK_EQ_INT(buf_find(&b, 0, "BODY", 4), 27);  /* 맨 끝 */
    CHECK_EQ_INT(buf_find(&b, 0, "BODYX", 5), -1); /* 끝에서 잘린 needle */
    CHECK_EQ_INT(buf_find(&b, 0, "nope", 4), -1);
    CHECK_EQ_INT(buf_find(&b, 3, "", 0), 3);        /* 빈 needle */
    CHECK_EQ_INT(buf_find(&b, b.len + 1, "G", 1), -1); /* from 범위 밖 */
    CHECK_EQ_INT(buf_find(&b, b.len, "", 0), (ssize_t)b.len);
    buf_free(&b);
}

/* "\r\n\r" 처럼 앞부분만 겹치는 가짜 후보를 건너뛰는지. */
static void test_find_partial_matches(void)
{
    buf_t b;
    const char *s = "a\r\n\rb\r\r\n\r\n";

    buf_init(&b, 0, 64);
    buf_append(&b, s, strlen(s));
    CHECK_EQ_INT(buf_find(&b, 0, "\r\n\r\n", 4), 6); /* 1,3 의 "\r\n\r" 와 5 의 "\r\r" 는 가짜 */
    buf_free(&b);
}

/* 바이너리 데이터(중간에 NUL)에서도 동작. */
static void test_find_binary(void)
{
    buf_t b;
    const char data[] = {'x', '\0', '\r', '\n', '\0', '\r', '\n', '\r', '\n'};

    buf_init(&b, 0, 64);
    buf_append(&b, data, sizeof data);
    CHECK_EQ_SIZE(b.len, sizeof data);
    CHECK_EQ_MEM(b.data, data, sizeof data);
    CHECK_EQ_INT(buf_find(&b, 0, "\r\n\r\n", 4), 5);
    CHECK_EQ_INT(buf_find(&b, 0, "\0\r", 2), 1);
    buf_free(&b);
}

/*
 * 실제 사용 패턴: 1바이트씩 도착할 때 from 을 앞으로 옮기며 재검색해도
 * 경계에 걸친 "\r\n\r\n" 을 정확히 한 번, 올바른 위치에서 찾는다.
 */
static void test_find_incremental(void)
{
    buf_t b;
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\nrest";
    size_t i;
    size_t scanned = 0;
    ssize_t found = -1;
    size_t found_at_len = 0;

    buf_init(&b, 0, 1024);
    for (i = 0; req[i] != '\0' && found < 0; i++) {
        size_t from = scanned >= 3 ? scanned - 3 : 0;
        buf_append(&b, &req[i], 1);
        found = buf_find(&b, from, "\r\n\r\n", 4);
        scanned = b.len;
        found_at_len = b.len;
    }
    CHECK_EQ_INT(found, 23);
    CHECK_EQ_SIZE(found_at_len, 27); /* 마지막 '\n' 이 도착한 순간 발견 */
    buf_free(&b);
}

static void test_reset_reuse(void)
{
    buf_t b;
    size_t cap;

    buf_init(&b, 0, 64);
    buf_append(&b, "hello", 5);
    cap = b.cap;
    buf_reset(&b);
    CHECK_EQ_SIZE(b.len, 0);
    CHECK_EQ_STR(b.data, "");
    CHECK_EQ_SIZE(b.cap, cap); /* 메모리는 재사용 */
    buf_append(&b, "world", 5);
    CHECK_EQ_STR(b.data, "world");
    buf_free(&b);
}

int main(void)
{
    RUN_TEST(test_init_free);
    RUN_TEST(test_init_lazy_and_invalid);
    RUN_TEST(test_append_basic);
    RUN_TEST(test_append_grows);
    RUN_TEST(test_append_limit);
    RUN_TEST(test_appendf);
    RUN_TEST(test_consume);
    RUN_TEST(test_consume_pipelined);
    RUN_TEST(test_find_basic);
    RUN_TEST(test_find_partial_matches);
    RUN_TEST(test_find_binary);
    RUN_TEST(test_find_incremental);
    RUN_TEST(test_reset_reuse);
    TEST_END();
}
