/*
 * test_fproxy.c — 캐시 키 정규화, authority 분해, 저장 가능 판정(테이블).
 */
#include "test.h"

#include "fproxy.h"
#include "http_parser.h"

static http_parser_t parser;

static int parse(const char *raw, http_request_t *req)
{
    size_t used;

    http_parser_init(&parser, NULL);
    return http_parse(&parser, raw, strlen(raw), req, &used) == PARSE_OK ? 0 : -1;
}

static void test_cache_key(void)
{
    static const struct {
        const char *target;
        const char *key; /* NULL = 실패 기대 */
    } cases[] = {
        {"http://example.com/", "http://example.com/"},
        {"http://EXAMPLE.Com/Path", "http://example.com/Path"},   /* host 만 소문자, 경로는 보존 */
        {"http://example.com:80/a", "http://example.com/a"},       /* 기본 포트 생략 */
        {"http://example.com:8080/a?b=1", "http://example.com:8080/a?b=1"},
        {"http://example.com", "http://example.com/"},             /* 경로 없음 → "/" */
        {"http://example.com?q", "http://example.com/?q"},
        {"http://[::1]:8001/x", "http://[::1]:8001/x"},
        {"http://[::1]/x", "http://[::1]/x"},
        {"http://h:0/", NULL},
        {"http://h:99999/", NULL},
        {"http://h:/", NULL},
        {"http://h:8a/", NULL},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char raw[512], key[512];
        http_request_t req;
        int n;

        snprintf(raw, sizeof raw, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", cases[i].target);
        REQUIRE(parse(raw, &req) == 0);
        n = fproxy_cache_key(&req, key, sizeof key);
        if (cases[i].key == NULL) {
            CHECK_EQ_INT(n, -1);
        } else {
            CHECK(n > 0);
            if (n > 0)
                CHECK_EQ_STR(key, cases[i].key);
        }
    }
    /* origin-form 요청에는 캐시 키가 없다 */
    {
        http_request_t req;
        char key[64];

        REQUIRE(parse("GET /x HTTP/1.1\r\nHost: a\r\n\r\n", &req) == 0);
        CHECK_EQ_INT(fproxy_cache_key(&req, key, sizeof key), -1);
    }
}

static void test_split_authority(void)
{
    char host[64];
    int port = 0;

    CHECK_EQ_INT(fproxy_split_authority("a.com", 5, host, sizeof host, &port), 0);
    CHECK_EQ_STR(host, "a.com");
    CHECK_EQ_INT(port, 80);
    CHECK_EQ_INT(fproxy_split_authority("a.com:8001", 10, host, sizeof host, &port), 0);
    CHECK_EQ_INT(port, 8001);
    CHECK_EQ_INT(fproxy_split_authority("[::1]:9", 7, host, sizeof host, &port), 0);
    CHECK_EQ_STR(host, "::1");
    CHECK_EQ_INT(port, 9);
    CHECK_EQ_INT(fproxy_split_authority("[::1]x", 6, host, sizeof host, &port), -1);
    CHECK_EQ_INT(fproxy_split_authority(":80", 3, host, sizeof host, &port), -1);
    CHECK_EQ_INT(fproxy_split_authority("", 0, host, sizeof host, &port), -1);
}

static void test_cacheable(void)
{
    static const struct {
        const char *name;
        const char *req;
        const char *head; /* 상태줄+헤더 (빈 줄 없음) */
        int ok;
        long long ttl;
    } cases[] = {
        {"기본 200 + CL → 기본 TTL", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 1, 60000},
        {"max-age", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: public, max-age=30\r\n", 1, 30000},
        {"s-maxage 가 max-age 보다 우선", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: max-age=30, s-maxage=5\r\n", 1, 5000},
        {"no-store", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: no-store\r\n", 0, 0},
        {"private (대소문자)", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: max-age=60, PRIVATE\r\n", 0, 0},
        {"private=\"field\"", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: private=\"X\"\r\n", 0, 0},
        {"no-cache", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: no-cache\r\n", 0, 0},
        {"max-age=0", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: max-age=0\r\n", 0, 0},
        {"no-storex 는 no-store 아님", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: no-storex\r\n", 1, 60000},
        {"두 번째 Cache-Control 줄도 본다", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: max-age=9\r\nCache-Control: no-store\r\n", 0, 0},
        {"Set-Cookie", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nSet-Cookie: s=1\r\n", 0, 0},
        {"404", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\n", 0, 0},
        {"CL 없음", "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n", "HTTP/1.1 200 OK\r\n", 0, 0},
        {"POST", "POST http://h/ HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 0, 0},
        {"HEAD 는 저장하지 않음", "HEAD http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 0, 0},
        {"Authorization 요청", "GET http://h/ HTTP/1.1\r\nHost: h\r\nAuthorization: Basic x\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 0, 0},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request_t req;
        long long ttl = -1;
        int ok;

        REQUIRE(parse(cases[i].req, &req) == 0);
        ok = fproxy_cacheable(&req, cases[i].head, strlen(cases[i].head), 60000, &ttl);
        if (ok != cases[i].ok)
            fprintf(stderr, "    [%s] ok=%d\n", cases[i].name, ok);
        CHECK_EQ_INT(ok, cases[i].ok);
        if (ok)
            CHECK_EQ_INT(ttl, cases[i].ttl);
    }
    /* 기본 TTL 이 0 이고 max-age 도 없으면 저장하지 않음 */
    {
        http_request_t req;
        long long ttl;

        REQUIRE(parse("GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n", &req) == 0);
        CHECK_EQ_INT(fproxy_cacheable(&req, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n", 37, 0, &ttl), 0);
    }
}

int main(void)
{
    RUN_TEST(test_cache_key);
    RUN_TEST(test_split_authority);
    RUN_TEST(test_cacheable);
    TEST_END();
}
