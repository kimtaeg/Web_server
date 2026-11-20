/*
 * test_static.c — 경로 처리 순수 함수(테이블) + 임시 디렉터리에서 static_handle 통합 검사.
 */
#include "test.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buf.h"
#include "http_parser.h"
#include "netio.h"
#include "static.h"

/* ------------------------------------------------------------------ */
/* 순수 함수                                                           */
/* ------------------------------------------------------------------ */

static void test_url_decode(void)
{
    static const struct {
        const char *in;
        int want; /* 길이 또는 STATIC_EBAD */
        const char *out;
    } cases[] = {
        {"/a/b", 4, "/a/b"},
        {"/%2e%2e/x", 5, "/../x"},
        {"/%2E%2E%2Fetc", 7, "/../etc"}, /* %2F 도 '/' 로 디코딩된다 → 정규화가 처리 */
        {"/a%20b", 4, "/a b"},
        {"/a+b", 4, "/a+b"},     /* '+' 는 경로에서 그대로 */
        {"/%41%42", 3, "/AB"},
        {"/%ff", 2, "/\xff"},
        {"/%00", STATIC_EBAD, NULL}, /* NUL 주입 */
        {"/a%00.txt", STATIC_EBAD, NULL},
        {"/%zz", STATIC_EBAD, NULL},
        {"/%2", STATIC_EBAD, NULL}, /* 잘린 인코딩 */
        {"/%", STATIC_EBAD, NULL},
        {"/%g0", STATIC_EBAD, NULL},
        {"", 0, ""},
    };
    size_t i;
    char out[64];

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int n = static_url_decode(cases[i].in, strlen(cases[i].in), out, sizeof out);

        CHECK_EQ_INT(n, cases[i].want);
        if (cases[i].out != NULL && n >= 0)
            CHECK_EQ_STR(out, cases[i].out);
    }

    /* 출력 버퍼 부족 */
    CHECK_EQ_INT(static_url_decode("/abcdef", 7, out, 4), STATIC_ETOOLONG);
    CHECK_EQ_INT(static_url_decode("/abc", 4, out, 5), 4); /* 정확히 맞음 (NUL 포함 5) */
    CHECK_EQ_INT(static_url_decode("/abc", 4, out, 0), STATIC_ETOOLONG);
}

static void test_normalize(void)
{
    static const struct {
        const char *in;
        int rc;
        const char *out;
    } cases[] = {
        {"/", 0, "/"},
        {"/a/b", 0, "/a/b"},
        {"/a/b/", 0, "/a/b"},
        {"//a///b//", 0, "/a/b"},         /* "//" 합치기 */
        {"/./a/./b/.", 0, "/a/b"},
        {"/a/b/../c", 0, "/a/c"},
        {"/a/../b/../c", 0, "/c"},
        {"/a/..", 0, "/"},
        {"/a/../", 0, "/"},
        {"/...", 0, "/..."},              /* "..." 는 평범한 이름 */
        {"/..a/b..", 0, "/..a/b.."},
        {"/a/.b", 0, "/a/.b"},
        {"/..", -1, NULL},                /* "../" 탈출 */
        {"/../etc/passwd", -1, NULL},
        {"/a/../../etc", -1, NULL},
        {"/a/b/../../../x", -1, NULL},
        {"//..//", -1, NULL},
        {"/./..", -1, NULL},
        {"relative", -1, NULL},           /* '/' 로 시작해야 함 */
        {"", -1, NULL},
    };
    size_t i;
    char p[64];

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int rc;

        snprintf(p, sizeof p, "%s", cases[i].in);
        rc = static_path_normalize(p);
        if (rc != cases[i].rc)
            fprintf(stderr, "    normalize(\"%s\")\n", cases[i].in);
        CHECK_EQ_INT(rc, cases[i].rc);
        if (rc == 0 && cases[i].out)
            CHECK_EQ_STR(p, cases[i].out);
    }
}

/* "%2e%2e" 는 디코딩 → 정규화 순서를 거쳐야 탈출로 판정된다. */
static void test_decode_then_normalize(void)
{
    static const char *escapes[] = {
        "/%2e%2e/etc/passwd", "/%2E%2E", "/a/%2e%2e/%2e%2e/x", "/%2e%2e%2f%2e%2e%2fetc",
        "/..%2fetc", "/.%2e/x",
    };
    size_t i;
    char p[64];

    for (i = 0; i < sizeof escapes / sizeof escapes[0]; i++) {
        REQUIRE(static_url_decode(escapes[i], strlen(escapes[i]), p, sizeof p) >= 0);
        if (static_path_normalize(p) != -1)
            fprintf(stderr, "    not rejected: %s\n", escapes[i]);
        CHECK_EQ_INT(static_path_normalize(p), -1);
    }
}

static void test_within(void)
{
    const char *root = "/var/www";
    size_t rl = strlen(root);

    CHECK(static_path_within(root, rl, "/var/www"));
    CHECK(static_path_within(root, rl, "/var/www/"));
    CHECK(static_path_within(root, rl, "/var/www/a/b.html"));
    CHECK(!static_path_within(root, rl, "/var/www2"));       /* 접두사 함정 */
    CHECK(!static_path_within(root, rl, "/var/www2/x"));
    CHECK(!static_path_within(root, rl, "/var/ww"));
    CHECK(!static_path_within(root, rl, "/var"));
    CHECK(!static_path_within(root, rl, "/etc/passwd"));
    CHECK(static_path_within("/", 1, "/anything"));
}

/* ------------------------------------------------------------------ */
/* static_handle 통합 (실제 파일시스템)                                 */
/* ------------------------------------------------------------------ */

static char base[256]; /* mkdtemp 결과 */
static static_ctx_t sctx;

static void write_file(const char *rel, const char *content)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", base, rel);
    f = fopen(p, "w");
    if (f == NULL)
        abort();
    fputs(content, f);
    fclose(f);
}

static void make_dir(const char *rel)
{
    char p[512];

    snprintf(p, sizeof p, "%s/%s", base, rel);
    mkdir(p, 0755);
}

static void make_link(const char *target, const char *rel)
{
    char p[512];

    snprintf(p, sizeof p, "%s/%s", base, rel);
    if (symlink(target, p) != 0)
        abort();
}

/*
 * 트리:
 *   base/outside/secret.txt   "SECRET"
 *   base/root/index.html, a.txt, style.css
 *   base/root/docs/index.html
 *   base/root/empty/            (index.html 없음)
 *   base/root/sub/x.txt
 *   base/root/link_out  -> ../outside/secret.txt   (루트 밖 파일 링크)
 *   base/root/dir_out   -> ../outside              (루트 밖 디렉터리 링크)
 *   base/root/link_in   -> a.txt                   (루트 안 링크: 허용)
 *   base/root/noperm.txt (권한 000)
 */
static int setup_tree(void)
{
    char root[512];

    snprintf(base, sizeof base, "/tmp/cweb-test-XXXXXX");
    if (mkdtemp(base) == NULL)
        return -1;
    make_dir("outside");
    write_file("outside/secret.txt", "SECRET");
    make_dir("root");
    write_file("root/index.html", "<h1>home</h1>");
    write_file("root/a.txt", "hello");
    write_file("root/style.css", "body{}");
    make_dir("root/docs");
    write_file("root/docs/index.html", "docs-index");
    make_dir("root/empty");
    make_dir("root/sub");
    write_file("root/sub/x.txt", "xx");
    make_link("../outside/secret.txt", "root/link_out");
    make_link("../outside", "root/dir_out");
    make_link("a.txt", "root/link_in");
    write_file("root/noperm.txt", "nope");
    snprintf(root, sizeof root, "%s/root/noperm.txt", base);
    chmod(root, 0);

    snprintf(root, sizeof root, "%s/root", base);
    return static_init(&sctx, root);
}

static void teardown_tree(void)
{
    char cmd[600];

    snprintf(cmd, sizeof cmd, "chmod -R u+rwx '%s' && rm -rf '%s'", base, base);
    if (system(cmd) != 0)
        fprintf(stderr, "    정리 실패: %s\n", base);
}

/* raw 요청을 파싱해서 static_handle 에 넘기고, 응답 전체를 out 에 받는다. */
static int serve(const char *raw, buf_t *out, handler_result_t *res)
{
    http_parser_t p;
    http_request_t req;
    size_t used;
    int sv[2];
    char tmp[4096];
    ssize_t n;
    int rc;

    http_parser_init(&p, NULL);
    if (http_parse(&p, raw, strlen(raw), &req, &used) != PARSE_OK)
        return -100;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -101;
    memset(res, 0, sizeof *res);
    rc = static_handle(&sctx, sv[0], &req, 1, res);
    close(sv[0]);
    buf_reset(out);
    while ((n = recv_some(sv[1], tmp, sizeof tmp, 1000)) > 0)
        buf_append(out, tmp, (size_t)n);
    close(sv[1]);
    return rc;
}

static int status_of(const buf_t *b)
{
    if (b->len < 12 || strncmp(b->data, "HTTP/1.1 ", 9) != 0)
        return -1;
    return atoi(b->data + 9);
}

static const char *body_of(const buf_t *b)
{
    const char *p = strstr(b->data, "\r\n\r\n");

    return p ? p + 4 : "";
}

static void test_handle(void)
{
    static const struct {
        const char *path; /* GET 대상 */
        int status;
        const char *body;   /* NULL = 검사 안 함 */
        const char *header; /* 포함돼야 할 헤더 줄, NULL = 검사 안 함 */
    } cases[] = {
        {"/", 200, "<h1>home</h1>", "Content-Type: text/html; charset=utf-8\r\n"},
        {"/a.txt", 200, "hello", "Content-Type: text/plain; charset=utf-8\r\n"},
        {"/style.css", 200, "body{}", "Content-Type: text/css; charset=utf-8\r\n"},
        {"/a%2etxt", 200, "hello", NULL},
        {"/sub//x.txt", 200, "xx", NULL},          /* "//" */
        {"/sub/./../a.txt", 200, "hello", NULL},   /* 루트 안에서의 ".." 는 허용 */
        {"/link_in", 200, "hello", NULL},          /* 루트 안을 가리키는 링크 */
        {"/docs/", 200, "docs-index", NULL},
        {"/docs", 301, NULL, "Location: /docs/\r\n"},
        {"/docs?x=1", 301, NULL, "Location: /docs/?x=1\r\n"},
        {"/empty/", 403, NULL, NULL},              /* 디렉터리 목록 없음 */
        {"/nope", 404, NULL, NULL},
        {"/sub/nope/x", 404, NULL, NULL},
        {"/a.txt/", 404, NULL, NULL},
        {"/a.txt/x", 404, NULL, NULL},             /* ENOTDIR */
        /* 탈출 시도: 모두 403 */
        {"/../outside/secret.txt", 403, NULL, NULL},
        {"/%2e%2e/outside/secret.txt", 403, NULL, NULL},
        {"/%2E%2E%2Foutside%2Fsecret.txt", 403, NULL, NULL},
        {"/sub/%2e%2e/%2e%2e/outside/secret.txt", 403, NULL, NULL},
        {"/sub/../../outside/secret.txt", 403, NULL, NULL},
        {"/link_out", 403, NULL, NULL},            /* 루트 밖 파일 링크 */
        {"/dir_out/secret.txt", 403, NULL, NULL},  /* 루트 밖 디렉터리 링크 */
        /* 잘못된 인코딩 */
        {"/%zz", 400, NULL, NULL},
        {"/a%00.txt", 400, NULL, NULL},
    };
    buf_t out;
    size_t i;
    char raw[512];

    buf_init(&out, 0, 1 << 20);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        handler_result_t res;
        int rc;

        snprintf(raw, sizeof raw, "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", cases[i].path);
        rc = serve(raw, &out, &res);
        CHECK_EQ_INT(rc, 0);
        if (status_of(&out) != cases[i].status)
            fprintf(stderr, "    GET %s → %d (기대 %d)\n", cases[i].path, status_of(&out),
                    cases[i].status);
        CHECK_EQ_INT(status_of(&out), cases[i].status);
        CHECK_EQ_INT(res.status, cases[i].status);
        CHECK_EQ_SIZE(res.bytes, out.len);
        if (cases[i].body)
            CHECK_EQ_STR(body_of(&out), cases[i].body);
        if (cases[i].header)
            CHECK(strstr(out.data, cases[i].header) != NULL);
        CHECK(strstr(out.data, "SECRET") == NULL); /* 어떤 경우에도 루트 밖 내용 노출 금지 */
    }
    buf_free(&out);
}

static void test_head_and_methods(void)
{
    buf_t out;
    handler_result_t res;

    buf_init(&out, 0, 1 << 20);

    serve("HEAD /a.txt HTTP/1.1\r\nHost: a\r\n\r\n", &out, &res);
    CHECK_EQ_INT(status_of(&out), 200);
    CHECK(strstr(out.data, "\r\nContent-Length: 5\r\n") != NULL); /* GET 과 같은 길이 */
    CHECK_EQ_STR(body_of(&out), "");                              /* 본문 없음 */

    serve("HEAD /nope HTTP/1.1\r\nHost: a\r\n\r\n", &out, &res);
    CHECK_EQ_INT(status_of(&out), 404);
    CHECK_EQ_STR(body_of(&out), "");

    serve("POST /a.txt HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", &out, &res);
    CHECK_EQ_INT(status_of(&out), 405);
    CHECK(strstr(out.data, "\r\nAllow: GET, HEAD\r\n") != NULL);

    serve("DELETE /a.txt HTTP/1.1\r\nHost: a\r\n\r\n", &out, &res);
    CHECK_EQ_INT(status_of(&out), 405);

    /* keep_alive=1 로 호출했으므로 */
    serve("GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n", &out, &res);
    CHECK(strstr(out.data, "\r\nConnection: keep-alive\r\n") != NULL);
    buf_free(&out);
}

static void test_permission_denied(void)
{
    buf_t out;
    handler_result_t res;

    if (geteuid() == 0) { /* root 는 권한 000 파일도 읽을 수 있다 */
        printf("    (root 로 실행 중: 권한 검사 생략)\n");
        return;
    }
    buf_init(&out, 0, 1 << 20);
    serve("GET /noperm.txt HTTP/1.1\r\nHost: a\r\n\r\n", &out, &res);
    CHECK_EQ_INT(status_of(&out), 403);
    buf_free(&out);
}

static void test_init_errors(void)
{
    static_ctx_t s;
    char p[512];

    CHECK_EQ_INT(static_init(&s, "/definitely/not/here"), -1);
    snprintf(p, sizeof p, "%s/root/a.txt", base);
    CHECK_EQ_INT(static_init(&s, p), -1); /* 파일은 루트가 될 수 없다 */
}

int main(void)
{
    RUN_TEST(test_url_decode);
    RUN_TEST(test_normalize);
    RUN_TEST(test_decode_then_normalize);
    RUN_TEST(test_within);
    if (setup_tree() != 0) {
        fprintf(stderr, "임시 디렉터리 준비 실패\n");
        return 1;
    }
    RUN_TEST(test_handle);
    RUN_TEST(test_head_and_methods);
    RUN_TEST(test_permission_denied);
    RUN_TEST(test_init_errors);
    teardown_tree();
    TEST_END();
}
