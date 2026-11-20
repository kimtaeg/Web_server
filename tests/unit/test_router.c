/*
 * test_router.c — 접두사 매칭(경계, 최장 일치)과 디스패치.
 */
#include "test.h"

#include "router.h"

static int h_a(void *c, int fd, const http_request_t *r, const conn_info_t *ci, int ka,
               handler_result_t *res)
{
    (void)c; (void)fd; (void)r; (void)ci; (void)ka;
    res->status = 1;
    return 0;
}

static int h_b(void *c, int fd, const http_request_t *r, const conn_info_t *ci, int ka,
               handler_result_t *res)
{
    (void)c; (void)fd; (void)r; (void)ci; (void)ka;
    res->status = 2;
    return 0;
}

static int h_fallback(void *c, int fd, const http_request_t *r, const conn_info_t *ci, int ka,
                      handler_result_t *res)
{
    (void)c; (void)fd; (void)r; (void)ci; (void)ka;
    res->status = 9;
    return 0;
}

static int dispatch(router_t *rt, const char *path)
{
    http_request_t req;
    handler_result_t res;
    conn_info_t ci = {"-"};

    memset(&req, 0, sizeof req);
    memset(&res, 0, sizeof res);
    req.path = path;
    req.path_len = strlen(path);
    router_handle(rt, -1, &req, &ci, 1, &res);
    return res.status;
}

static void test_match(void)
{
    router_t rt;
    static const struct {
        const char *path;
        int want;
    } cases[] = {
        {"/cgi-bin", 1},
        {"/cgi-bin/", 1},
        {"/cgi-bin/adder", 1},
        {"/cgi-binx", 9},      /* 경계: 구성요소 중간에서 일치하면 안 됨 */
        {"/cgi-bin2/x", 9},
        {"/cgi", 9},
        {"/api/v1", 2},        /* 최장 일치: "/api/v1" 이 "/api" 보다 우선 */
        {"/api/v1/users", 2},
        {"/api/v10", 1 + 0 * 2}, /* "/api/v10" 은 "/api/v1" 과 경계 불일치 → "/api" */
        {"/api", 1},
        {"/", 9},
        {"/index.html", 9},
    };
    size_t i;

    router_init(&rt, h_fallback, NULL);
    CHECK_EQ_INT(router_add(&rt, "/cgi-bin/", h_a, NULL), 0); /* 끝 '/' 는 제거되어 저장 */
    CHECK_EQ_INT(router_add(&rt, "/api", h_a, NULL), 0);
    CHECK_EQ_INT(router_add(&rt, "/api/v1", h_b, NULL), 0);
    CHECK_EQ_STR(rt.routes[0].prefix, "/cgi-bin");
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = dispatch(&rt, cases[i].path);

        if (got != cases[i].want)
            fprintf(stderr, "    %s → %d (기대 %d)\n", cases[i].path, got, cases[i].want);
        CHECK_EQ_INT(got, cases[i].want);
    }
}

static void test_root_prefix_and_errors(void)
{
    router_t rt;
    int i;

    router_init(&rt, h_fallback, NULL);
    CHECK_EQ_INT(router_add(&rt, "/", h_b, NULL), 0); /* "/" = 전부 */
    CHECK_EQ_INT(dispatch(&rt, "/anything"), 2);
    CHECK_EQ_INT(router_add(&rt, "no-slash", h_a, NULL), -1);
    CHECK_EQ_INT(router_add(&rt, "", h_a, NULL), -1);
    CHECK_EQ_INT(router_add(&rt, "/x", NULL, NULL), -1);

    router_init(&rt, h_fallback, NULL);
    for (i = 0; i < ROUTER_MAX_ROUTES; i++)
        CHECK_EQ_INT(router_add(&rt, "/r", h_a, NULL), 0);
    CHECK_EQ_INT(router_add(&rt, "/overflow", h_a, NULL), -1); /* 가득 참 */
}

int main(void)
{
    RUN_TEST(test_match);
    RUN_TEST(test_root_prefix_and_errors);
    TEST_END();
}
