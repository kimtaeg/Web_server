/*
 * test_cgi.c — CGI 순수 함수(이름 검사, 출력 해석) + /bin/sh 스크립트로 실행 경로 검사.
 *   정상 / 종료 코드 ≠ 0 / exec 실패 / 타임아웃(+좀비·손자 프로세스 회수) / stdout 닫고 버팀
 *   / 큰 입력+큰 출력 동시(교착 없음) / 본문 안 읽음(EPIPE) / 환경 변수 / 핸들러 상태 코드
 */
#include "test.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "cgi.h"
#include "http_parser.h"
#include "netio.h"

/* ------------------------------------------------------------------ */
/* 순수 함수                                                           */
/* ------------------------------------------------------------------ */

static void test_script_name(void)
{
    static const struct {
        const char *name;
        int ok;
    } cases[] = {
        {"adder", 1}, {"echo.cgi", 1}, {"a-b_c.9", 1},
        {"", 0}, {".", 0}, {"..", 0}, {".hidden", 0},
        {"a/b", 0}, {"../x", 0}, {"%2e%2e", 0}, {"a b", 0}, {"a?b", 0}, {"a\\b", 0},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK_EQ_INT(cgi_script_name_ok(cases[i].name, strlen(cases[i].name)), cases[i].ok);
}

static void test_parse_output(void)
{
    static const struct {
        const char *out;
        int rc;
        int status;
        const char *headers; /* NULL = 검사 안 함 */
        const char *body;
    } cases[] = {
        {"Content-Type: text/plain\r\n\r\nhi", 0, 200, "Content-Type: text/plain\r\n", "hi"},
        {"Content-Type: text/plain\n\nhi", 0, 200, "Content-Type: text/plain\r\n", "hi"}, /* LF 만 */
        {"Status: 404 Not Found\nContent-Type: text/html\n\nnf", 0, 404, "Content-Type: text/html\r\n", "nf"},
        {"status: 201\ncontent-type: a/b\n\n", 0, 201, "content-type: a/b\r\n", ""},
        {"Location: /x\n\n", 0, 302, "Location: /x\r\n", ""},
        {"Status: 301 Moved\nLocation: /y\n\n", 0, 301, NULL, ""},
        {"Content-Type:   a/b   \nX-Custom: v\n\nB", 0, 200, "Content-Type: a/b\r\nX-Custom: v\r\n", "B"},
        /* 서버가 정하는 헤더는 버린다 */
        {"Content-Type: a/b\nContent-Length: 999\nConnection: close\nServer: evil\nDate: x\n\nB", 0,
         200, "Content-Type: a/b\r\n", "B"},
        /* 본문 안의 빈 줄은 본문 */
        {"Content-Type: a/b\n\nline1\n\nline2", 0, 200, NULL, "line1\n\nline2"},
        /* 오류 */
        {"X-Only: y\n\nbody", -1, 0, NULL, NULL},              /* Content-Type/Location 없음 */
        {"Content-Type: a/b\nbody without blank", -1, 0, NULL, NULL}, /* 빈 줄 없음 */
        {"", -1, 0, NULL, NULL},
        {"Status: abc\nContent-Type: a/b\n\n", -1, 0, NULL, NULL},
        {"Status: 99\nContent-Type: a/b\n\n", -1, 0, NULL, NULL},
        {"Status: 600 X\nContent-Type: a/b\n\n", -1, 0, NULL, NULL},
        {"Status: 2000\nContent-Type: a/b\n\n", -1, 0, NULL, NULL},
        {"NoColon\nContent-Type: a/b\n\n", -1, 0, NULL, NULL},
        {"Bad Name: v\nContent-Type: a/b\n\n", -1, 0, NULL, NULL},
        {"Content-Type: a\rb\n\n", -1, 0, NULL, NULL}, /* 값 속 CR: 응답 분할 방지 */
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        buf_t h;
        int st = 0;
        size_t off = 0;
        int rc;

        buf_init(&h, 0, 4096);
        rc = cgi_parse_output(cases[i].out, strlen(cases[i].out), &st, &h, &off);
        if (rc != cases[i].rc)
            fprintf(stderr, "    case %zu: rc=%d\n", i, rc);
        CHECK_EQ_INT(rc, cases[i].rc);
        if (rc == 0) {
            CHECK_EQ_INT(st, cases[i].status);
            if (cases[i].headers)
                CHECK_EQ_STR(h.data ? h.data : "", cases[i].headers);
            CHECK_EQ_STR(cases[i].out + off, cases[i].body);
        }
        buf_free(&h);
    }
}

/* ------------------------------------------------------------------ */
/* 실행 (임시 디렉터리의 /bin/sh 스크립트)                              */
/* ------------------------------------------------------------------ */

/*
 * 타임아웃 값 주의: macOS 는 새로 만든 실행 파일을 처음 실행할 때 보안 검사로 0.3~0.5초가 걸린다
 * (실측). 정상 완료를 기대하는 실행은 넉넉히(OK_MS), 타임아웃을 기대하는 실행은 sleep 30 처럼
 * 확실히 넘는 스크립트에 짧은 값(TO_MS)을 준다.
 */
#define OK_MS 10000
#define TO_MS 1000

static char dir[256];

static void script(const char *name, const char *body, int mode)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", dir, name);
    f = fopen(p, "w");
    if (f == NULL)
        abort();
    fputs(body, f);
    fclose(f);
    chmod(p, (mode_t)mode);
}

static char *spath(const char *name)
{
    static char p[512];

    snprintf(p, sizeof p, "%s/%s", dir, name);
    return p;
}

static long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* pid 가 회수되어 더 이상 존재하지 않는가 (좀비도 아님) */
static int gone(pid_t pid)
{
    return kill(pid, 0) == -1 && errno == ESRCH;
}

static int setup(void)
{
    snprintf(dir, sizeof dir, "/tmp/cweb-cgi-XXXXXX");
    if (mkdtemp(dir) == NULL)
        return -1;
    script("ok.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nhello'\n", 0755);
    script("exit3.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nx'\nexit 3\n", 0755);
    script("badshebang.sh", "#!/nonexistent/interpreter\n", 0755);
    script("slow.sh", "#!/bin/sh\nsleep 30\n", 0755);
    /* 손자 프로세스를 띄우고 그 pid 를 파일에 남긴 뒤 잔다 */
    script("grand.sh", "#!/bin/sh\nsleep 30 &\necho $! > grand.pid\nsleep 30\n", 0755);
    /* stdout 을 닫고 계속 살아 있음 → EOF 후에도 waitpid 가 영원히 막히면 안 됨 */
    script("closeout.sh", "#!/bin/sh\nprintf 'Content-Type: a/b\\n\\n'\nexec 1>&-\nsleep 30\n", 0755);
    /* 본문을 그대로 반사: 입력과 출력이 모두 파이프 버퍼보다 큼 */
    script("cat.sh", "#!/bin/sh\nprintf 'Content-Type: a/b\\n\\n'\nexec cat\n", 0755);
    /* 본문을 전혀 안 읽고 바로 끝남 → 서버의 write 가 EPIPE */
    script("noread.sh", "#!/bin/sh\nprintf 'Content-Type: a/b\\n\\nignored'\n", 0755);
    script("env.sh",
           "#!/bin/sh\nprintf 'Content-Type: a/b\\n\\n'\n"
           "echo \"M=$REQUEST_METHOD Q=$QUERY_STRING CL=$CONTENT_LENGTH CT=$CONTENT_TYPE\"\n"
           "echo \"S=$SCRIPT_NAME P=$SERVER_PROTOCOL R=$REMOTE_ADDR G=$GATEWAY_INTERFACE\"\n"
           "echo \"HOME=${HOME:-unset} PWD=$(pwd)\"\n",
           0755);
    script("noexec.sh", "#!/bin/sh\necho hi\n", 0644);
    script("status.sh", "#!/bin/sh\nprintf 'Status: 418 Teapot\\nContent-Type: a/b\\n\\ntea'\n", 0755);
    script("garbage.sh", "#!/bin/sh\necho no headers here\n", 0755);
    return 0;
}

static void teardown(void)
{
    char cmd[400];

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "    정리 실패: %s\n", dir);
}

static char *const no_env[] = {"PATH=/usr/bin:/bin", NULL};

static void test_run_ok(void)
{
    buf_t out;
    pid_t pid = -1;

    buf_init(&out, 0, 1 << 20);
    CHECK_EQ_INT(cgi_run(spath("ok.sh"), no_env, NULL, 0, OK_MS, 1 << 20, &out, &pid), CGI_OK);
    CHECK(out.len > 0 && strstr(out.data, "hello") != NULL);
    CHECK(gone(pid)); /* 좀비 없음 */
    buf_free(&out);
}

static void test_run_failures(void)
{
    buf_t out;
    pid_t pid = -1;

    buf_init(&out, 0, 1 << 20);
    CHECK_EQ_INT(cgi_run(spath("exit3.sh"), no_env, NULL, 0, OK_MS, 1 << 20, &out, &pid), CGI_ERR_EXIT);
    CHECK(gone(pid));
    buf_reset(&out);
    CHECK_EQ_INT(cgi_run(spath("badshebang.sh"), no_env, NULL, 0, OK_MS, 1 << 20, &out, &pid),
                 CGI_ERR_EXEC);
    CHECK(gone(pid));
    buf_reset(&out);
    CHECK_EQ_INT(cgi_run(spath("does-not-exist"), no_env, NULL, 0, OK_MS, 1 << 20, &out, &pid),
                 CGI_ERR_EXEC);
    CHECK(gone(pid));
    buf_free(&out);
}

static void test_run_timeout_reaps(void)
{
    buf_t out;
    pid_t pid = -1;
    long t0;
    char gp[512];
    FILE *f;
    long grand = 0;

    buf_init(&out, 0, 1 << 20);
    t0 = ms_now();
    CHECK_EQ_INT(cgi_run(spath("slow.sh"), no_env, NULL, 0, TO_MS, 1 << 20, &out, &pid), CGI_ERR_TIMEOUT);
    CHECK(ms_now() - t0 < TO_MS + 1500);
    CHECK(gone(pid));

    /* 손자 프로세스까지 프로세스 그룹째 죽었는가 */
    CHECK_EQ_INT(cgi_run(spath("grand.sh"), no_env, NULL, 0, 3000, 1 << 20, &out, &pid), CGI_ERR_TIMEOUT);
    CHECK(gone(pid));
    snprintf(gp, sizeof gp, "%s/grand.pid", dir);
    f = fopen(gp, "r");
    REQUIRE(f != NULL);
    if (fscanf(f, "%ld", &grand) != 1)
        grand = 0;
    fclose(f);
    REQUIRE(grand > 0);
    {
        int i;
        /* 손자는 init 이 회수하므로 잠깐 기다린다 */
        for (i = 0; i < 100 && !gone((pid_t)grand); i++) {
            struct timespec ts = {0, 10 * 1000 * 1000};

            nanosleep(&ts, NULL);
        }
    }
    CHECK(gone((pid_t)grand));

    /* stdout 을 닫고 버티는 CGI: EOF 이후에도 타임아웃이 적용되어야 한다 */
    t0 = ms_now();
    CHECK_EQ_INT(cgi_run(spath("closeout.sh"), no_env, NULL, 0, TO_MS, 1 << 20, &out, &pid),
                 CGI_ERR_TIMEOUT);
    CHECK(ms_now() - t0 < TO_MS + 1500);
    CHECK(gone(pid));
    buf_free(&out);
}

/* 1MB 입력을 쓰는 동안 CGI 가 1MB 를 출력 → 순차 처리라면 교착. */
static void test_run_big_io_no_deadlock(void)
{
    size_t n = 1 << 20;
    char *body = malloc(n);
    buf_t out;
    pid_t pid = -1;
    size_t i;
    int rc;

    REQUIRE(body != NULL);
    for (i = 0; i < n; i++)
        body[i] = (char)(i * 7 + 3);
    buf_init(&out, 0, 4 << 20);
    rc = cgi_run(spath("cat.sh"), no_env, body, n, OK_MS, 4 << 20, &out, &pid);
    CHECK_EQ_INT(rc, CGI_OK);
    CHECK_EQ_SIZE(out.len, n + strlen("Content-Type: a/b\n\n"));
    if (out.len == n + strlen("Content-Type: a/b\n\n"))
        CHECK_EQ_MEM(out.data + strlen("Content-Type: a/b\n\n"), body, n);
    CHECK(gone(pid));

    /* 출력 상한 초과 */
    buf_reset(&out);
    CHECK_EQ_INT(cgi_run(spath("cat.sh"), no_env, body, n, OK_MS, 1000, &out, &pid), CGI_ERR_TOO_BIG);
    CHECK(gone(pid));

    /* 본문을 읽지 않는 CGI: 서버는 EPIPE 를 받고도 출력을 정상 수집 */
    buf_reset(&out);
    CHECK_EQ_INT(cgi_run(spath("noread.sh"), no_env, body, n, OK_MS, 1 << 20, &out, &pid), CGI_OK);
    CHECK(strstr(out.data, "ignored") != NULL);
    buf_free(&out);
    free(body);
}

/* ------------------------------------------------------------------ */
/* cgi_handle (HTTP 응답 수준)                                          */
/* ------------------------------------------------------------------ */

static cgi_ctx_t cctx;

static int handle(const char *raw, buf_t *resp)
{
    http_parser_t p;
    http_request_t req;
    handler_result_t res;
    conn_info_t ci = {"203.0.113.9"};
    size_t used;
    int sv[2];
    char tmp[8192];
    ssize_t n;

    http_parser_init(&p, NULL);
    if (http_parse(&p, raw, strlen(raw), &req, &used) != PARSE_OK)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;
    memset(&res, 0, sizeof res);
    cgi_handle(&cctx, sv[0], &req, &ci, 1, &res);
    close(sv[0]);
    buf_reset(resp);
    while ((n = recv_some(sv[1], tmp, sizeof tmp, 2000)) > 0)
        buf_append(resp, tmp, (size_t)n);
    close(sv[1]);
    return resp->len > 12 ? atoi(resp->data + 9) : -1;
}

static void test_handle_statuses(void)
{
    static const struct {
        const char *raw;
        int status;
        const char *contains;
    } cases[] = {
        {"GET /cgi-bin/ok.sh HTTP/1.1\r\nHost: a\r\n\r\n", 200, "\r\n\r\nhello"},
        {"GET /cgi-bin/status.sh HTTP/1.1\r\nHost: a\r\n\r\n", 418, "tea"},
        {"GET /cgi-bin/exit3.sh HTTP/1.1\r\nHost: a\r\n\r\n", 500, NULL},
        {"GET /cgi-bin/badshebang.sh HTTP/1.1\r\nHost: a\r\n\r\n", 500, NULL},
        {"GET /cgi-bin/garbage.sh HTTP/1.1\r\nHost: a\r\n\r\n", 500, NULL},
        {"GET /cgi-bin/slow.sh HTTP/1.1\r\nHost: a\r\n\r\n", 504, NULL},
        {"GET /cgi-bin/noexec.sh HTTP/1.1\r\nHost: a\r\n\r\n", 403, NULL},
        {"GET /cgi-bin/nope HTTP/1.1\r\nHost: a\r\n\r\n", 404, NULL},
        {"GET /cgi-bin/ HTTP/1.1\r\nHost: a\r\n\r\n", 404, NULL},
        {"GET /cgi-bin/../ok.sh HTTP/1.1\r\nHost: a\r\n\r\n", 404, NULL},
        {"GET /cgi-bin/%2e%2e HTTP/1.1\r\nHost: a\r\n\r\n", 404, NULL},
        {"GET /cgi-bin/sub/ok.sh HTTP/1.1\r\nHost: a\r\n\r\n", 404, NULL},
        {"PUT /cgi-bin/ok.sh HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", 405,
         "Allow: GET, HEAD, POST\r\n"},
        {"HEAD /cgi-bin/ok.sh HTTP/1.1\r\nHost: a\r\n\r\n", 200, "Content-Length: 5\r\n"},
    };
    buf_t resp;
    size_t i;

    buf_init(&resp, 0, 1 << 20);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int st = handle(cases[i].raw, &resp);

        if (st != cases[i].status)
            fprintf(stderr, "    %.40s → %d\n", cases[i].raw, st);
        CHECK_EQ_INT(st, cases[i].status);
        if (cases[i].contains)
            CHECK(strstr(resp.data, cases[i].contains) != NULL);
    }
    /* HEAD 는 본문 없음 */
    handle("HEAD /cgi-bin/ok.sh HTTP/1.1\r\nHost: a\r\n\r\n", &resp);
    CHECK_EQ_STR(strstr(resp.data, "\r\n\r\n") + 4, "");
    /* 서버가 붙이는 Content-Length 는 실제 본문 길이 */
    handle("GET /cgi-bin/ok.sh HTTP/1.1\r\nHost: a\r\n\r\n", &resp);
    CHECK(strstr(resp.data, "Content-Length: 5\r\n") != NULL);
    buf_free(&resp);
}

static void test_handle_env(void)
{
    buf_t resp;
    char want[PATH_MAX + 16]; /* cctx.dir 는 PATH_MAX 까지 (gcc -Wformat-truncation) */

    buf_init(&resp, 0, 1 << 20);
    handle("POST /cgi-bin/env.sh?x=1&y=2 HTTP/1.1\r\nHost: a\r\nContent-Type: text/plain\r\n"
           "Content-Length: 3\r\n\r\nabc",
           &resp);
    CHECK(strstr(resp.data, "M=POST Q=x=1&y=2 CL=3 CT=text/plain\n") != NULL);
    CHECK(strstr(resp.data, "S=/cgi-bin/env.sh P=HTTP/1.1 R=203.0.113.9 G=CGI/1.1\n") != NULL);
    CHECK(strstr(resp.data, "HOME=unset") != NULL); /* 서버 환경 변수는 새지 않음 */
    snprintf(want, sizeof want, "PWD=%s\n", cctx.dir); /* 스크립트 디렉터리에서 실행 */
    CHECK(strstr(resp.data, want) != NULL);

    handle("GET /cgi-bin/env.sh HTTP/1.0\r\n\r\n", &resp);
    CHECK(strstr(resp.data, "M=GET Q= CL= CT=\n") != NULL); /* GET: CONTENT_LENGTH 없음 */
    CHECK(strstr(resp.data, "P=HTTP/1.0") != NULL);
    buf_free(&resp);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* 서버(main)와 같은 전제 — cgi.h 참고 */
    RUN_TEST(test_script_name);
    RUN_TEST(test_parse_output);
    if (setup() != 0 || cgi_init(&cctx, dir, "/cgi-bin/", 3000) != 0) {
        fprintf(stderr, "준비 실패\n");
        return 1;
    }
    RUN_TEST(test_run_ok);
    RUN_TEST(test_run_failures);
    RUN_TEST(test_run_timeout_reaps);
    RUN_TEST(test_run_big_io_no_deadlock);
    RUN_TEST(test_handle_statuses);
    RUN_TEST(test_handle_env);
    teardown();
    TEST_END();
}
