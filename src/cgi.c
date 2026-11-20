/*
 * cgi.c — CGI 실행. 설계(멀티스레드 fork, fd 누수, 교착, 좀비)는 cgi.h 참고.
 *
 * 동기화: cgi_ctx_t 는 시작 시 한 번 쓰고 이후 읽기만 한다. 요청마다의 상태는 모두 스택/지역.
 */
#include "cgi.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "http_response.h"
#include "log.h"
#include "netio.h"

#define CGI_IO_CHUNK 16384
#define CGI_MAX_HEADERS_BYTES (64 * 1024)
#define CGI_REAP_POLL_MS 5
#define CGI_ENV_MAX 16

/* ------------------------------------------------------------------ */
/* 순수 함수                                                           */
/* ------------------------------------------------------------------ */

int cgi_script_name_ok(const char *name, size_t n)
{
    size_t i;

    if (n == 0 || name[0] == '.') /* ".", "..", 숨김 파일 */
        return 0;
    for (i = 0; i < n; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int span_ieq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && strncasecmp(s, lit, n) == 0;
}

int cgi_parse_output(const char *out, size_t n, int *status, buf_t *headers, size_t *body_off)
{
    size_t pos = 0;
    int st = 0;
    int has_ct = 0;
    int has_loc = 0;

    for (;;) {
        const char *nl = memchr(out + pos, '\n', n - pos);
        size_t ls = pos;
        size_t le;
        size_t colon;
        size_t vs;
        size_t ve;
        size_t i;

        if (nl == NULL)
            return -1; /* 헤더 끝(빈 줄)이 없다 */
        le = (size_t)(nl - out);
        pos = le + 1;
        if (le > ls && out[le - 1] == '\r')
            le--;
        if (le == ls) /* 빈 줄 → 본문 시작 */
            break;

        for (colon = ls; colon < le && out[colon] != ':'; colon++)
            if (out[colon] <= ' ' || out[colon] == 0x7f)
                return -1;
        if (colon == le || colon == ls)
            return -1;
        vs = colon + 1;
        ve = le;
        while (vs < ve && (out[vs] == ' ' || out[vs] == '\t'))
            vs++;
        while (ve > vs && (out[ve - 1] == ' ' || out[ve - 1] == '\t'))
            ve--;
        for (i = vs; i < ve; i++) /* CR/LF 등 제어문자가 값에 있으면 응답 분할(header injection) 위험 */
            if (((unsigned char)out[i] < 0x20 && out[i] != '\t') || out[i] == 0x7f)
                return -1;

        if (span_ieq(out + ls, colon - ls, "status")) {
            /* "Status: 404 Not Found" — 세 자리 숫자 + (공백 + 사유) */
            if (ve - vs < 3 || out[vs] < '1' || out[vs] > '5' || out[vs + 1] < '0' ||
                out[vs + 1] > '9' || out[vs + 2] < '0' || out[vs + 2] > '9' ||
                (ve - vs > 3 && out[vs + 3] != ' '))
                return -1;
            st = (out[vs] - '0') * 100 + (out[vs + 1] - '0') * 10 + (out[vs + 2] - '0');
            continue;
        }
        if (span_ieq(out + ls, colon - ls, "connection") ||
            span_ieq(out + ls, colon - ls, "content-length") ||
            span_ieq(out + ls, colon - ls, "transfer-encoding") ||
            span_ieq(out + ls, colon - ls, "keep-alive") || span_ieq(out + ls, colon - ls, "date") ||
            span_ieq(out + ls, colon - ls, "server"))
            continue; /* 서버가 직접 정한다 */
        if (span_ieq(out + ls, colon - ls, "content-type"))
            has_ct = 1;
        else if (span_ieq(out + ls, colon - ls, "location"))
            has_loc = 1;
        if (buf_appendf(headers, "%.*s: %.*s\r\n", (int)(colon - ls), out + ls, (int)(ve - vs),
                        out + vs) != BUF_OK)
            return -1;
    }
    if (!has_ct && !has_loc)
        return -1;
    if (st == 0)
        st = has_loc ? 302 : 200;
    *status = st;
    *body_off = pos;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 실행                                                                */
/* ------------------------------------------------------------------ */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int set_flags(int fd, int cloexec, int nonblock)
{
    if (cloexec && fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
        return -1;
    if (nonblock) {
        int fl = fcntl(fd, F_GETFL);

        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
            return -1;
    }
    return 0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* 프로세스 그룹 전체를 죽이고 회수한다. */
static void kill_and_reap(pid_t pid, int *wstatus)
{
    kill(-pid, SIGKILL); /* 그룹 (손자 프로세스 포함) */
    kill(pid, SIGKILL);  /* setpgid 가 늦었을 경우 대비 */
    while (waitpid(pid, wstatus, 0) < 0 && errno == EINTR)
        ;
}

/*
 * 자식 프로세스: fork 와 execve 사이. async-signal-safe 함수만 쓴다 (cgi.h 참고).
 * 실패하면 종료 코드로만 알린다: 126 = 준비 실패, 127 = exec 실패.
 */
static void child_exec(const char *path, const char *dir, int in_fd, int out_fd,
                       char *const argv[], char *const envp[], const struct sigaction *dfl)
{
    setpgid(0, 0); /* 자기 프로세스 그룹: 타임아웃 시 그룹째 kill, 터미널 Ctrl+C 도 안 받음 */
    /* 서버가 무시(SIG_IGN)하던 SIGPIPE 는 exec 후에도 "무시"로 상속되므로 기본값으로 되돌린다 */
    sigaction(SIGPIPE, dfl, NULL);
    if (dup2(in_fd, STDIN_FILENO) < 0 || dup2(out_fd, STDOUT_FILENO) < 0)
        _exit(126);
    /* 원래 파이프 fd 들은 CLOEXEC 라 exec 시 닫힌다. dup2 로 만든 0/1 은 CLOEXEC 가 없다. */
    if (chdir(dir) != 0)
        _exit(126);
    execve(path, argv, envp);
    _exit(127);
}

int cgi_run(const char *path, char *const envp[], const char *body, size_t body_len, int timeout_ms,
            size_t max_output, buf_t *out, pid_t *pid_out)
{
    int in_p[2] = {-1, -1};
    int out_p[2] = {-1, -1};
    char dir[PATH_MAX];
    char *argv[2];
    struct sigaction dfl;
    char *tmp = NULL;
    const char *slash;
    pid_t pid = -1;
    int rc = CGI_ERR_INTERNAL;
    int wstatus = 0;
    int reaped = 0;
    size_t written = 0;
    long deadline;

    /* fork 전에 모두 준비: 작업 디렉터리 문자열, argv, 시그널 설정, 읽기 버퍼 */
    slash = strrchr(path, '/');
    if (slash == NULL || (size_t)(slash - path) >= sizeof dir)
        return CGI_ERR_INTERNAL;
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
    if (dir[0] == '\0')
        snprintf(dir, sizeof dir, "/");
    tmp = malloc(CGI_IO_CHUNK);
    if (tmp == NULL)
        return CGI_ERR_INTERNAL;
    argv[0] = (char *)path;
    argv[1] = NULL;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);

    if (pipe(in_p) != 0 || pipe(out_p) != 0)
        goto cleanup;
    if (set_flags(in_p[0], 1, 0) || set_flags(in_p[1], 1, 1) || set_flags(out_p[0], 1, 1) ||
        set_flags(out_p[1], 1, 0))
        goto cleanup;

    pid = fork();
    if (pid < 0)
        goto cleanup;
    if (pid == 0)
        child_exec(path, dir, in_p[0], out_p[1], argv, envp, &dfl); /* 돌아오지 않는다 */

    setpgid(pid, pid); /* 자식과 경쟁하지 않도록 부모도 설정 (이미 exec 했으면 실패해도 무방) */
    if (pid_out != NULL)
        *pid_out = pid;
    close_fd(&in_p[0]);
    close_fd(&out_p[1]); /* 부모가 쓰기 끝을 들고 있으면 자식이 끝나도 EOF 가 안 온다 */
    if (body_len == 0)
        close_fd(&in_p[1]); /* 본문 없음 → stdin 즉시 EOF */

    deadline = now_ms() + timeout_ms;
    for (;;) { /* stdin 쓰기와 stdout 읽기를 한 루프에서 (교착 방지) */
        struct pollfd pfd[2];
        int npfd = 0;
        int out_i;
        int in_i = -1;
        long left = deadline - now_ms();
        int pr;

        if (left <= 0) {
            rc = CGI_ERR_TIMEOUT;
            goto kill_child;
        }
        out_i = npfd;
        pfd[npfd].fd = out_p[0];
        pfd[npfd].events = POLLIN;
        pfd[npfd++].revents = 0;
        if (in_p[1] >= 0) {
            in_i = npfd;
            pfd[npfd].fd = in_p[1];
            pfd[npfd].events = POLLOUT;
            pfd[npfd++].revents = 0;
        }
        pr = poll(pfd, (nfds_t)npfd, (int)left);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            goto kill_child;
        }
        if (pr == 0)
            continue; /* 다음 반복에서 타임아웃 판정 */

        if (in_i >= 0 && pfd[in_i].revents != 0) {
            ssize_t w = write(in_p[1], body + written, body_len - written);

            if (w > 0) {
                written += (size_t)w;
                if (written == body_len)
                    close_fd(&in_p[1]);
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                close_fd(&in_p[1]); /* EPIPE: CGI 가 본문을 다 안 읽고 닫음. 출력은 계속 받는다 */
            }
        }
        if (pfd[out_i].revents != 0) {
            ssize_t r = read(out_p[0], tmp, CGI_IO_CHUNK);

            if (r > 0) {
                if (out->len + (size_t)r > max_output || buf_append(out, tmp, (size_t)r) != BUF_OK) {
                    rc = CGI_ERR_TOO_BIG;
                    goto kill_child;
                }
            } else if (r == 0) {
                break; /* stdout EOF */
            } else if (errno != EAGAIN && errno != EINTR) {
                goto kill_child;
            }
        }
    }

    /* stdout 은 닫혔지만 프로세스가 아직 살아 있을 수 있다 (닫고 나서 sleep 하는 CGI).
     * 블로킹 waitpid 는 영원히 기다릴 수 있으므로 마감 시각까지만 WNOHANG 으로 확인한다. */
    for (;;) {
        pid_t w = waitpid(pid, &wstatus, WNOHANG);

        if (w == pid) {
            reaped = 1;
            break;
        }
        if (w < 0 && errno != EINTR) {
            reaped = 1; /* ECHILD: 이미 회수됨 (일어나면 안 되지만 무한 루프 방지) */
            rc = CGI_ERR_INTERNAL;
            goto cleanup;
        }
        if (now_ms() >= deadline) {
            rc = CGI_ERR_TIMEOUT;
            goto kill_child;
        }
        {
            struct timespec ts = {0, CGI_REAP_POLL_MS * 1000 * 1000};

            nanosleep(&ts, NULL);
        }
    }
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
        rc = CGI_OK;
    else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127)
        rc = CGI_ERR_EXEC;
    else
        rc = CGI_ERR_EXIT;
    goto cleanup;

kill_child:
    kill_and_reap(pid, &wstatus);
    reaped = 1;

cleanup:
    if (pid > 0 && !reaped) /* 위의 모든 경로가 회수하지만, 혹시 모를 좀비 방지 */
        kill_and_reap(pid, &wstatus);
    close_fd(&in_p[0]);
    close_fd(&in_p[1]);
    close_fd(&out_p[0]);
    close_fd(&out_p[1]);
    free(tmp);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 핸들러                                                              */
/* ------------------------------------------------------------------ */

int cgi_init(cgi_ctx_t *c, const char *dir, const char *url_prefix, int timeout_ms)
{
    struct stat st;
    size_t pl = strlen(url_prefix);

    memset(c, 0, sizeof *c);
    if (realpath(dir, c->dir) == NULL || stat(c->dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    c->dir_len = strlen(c->dir);
    while (pl > 0 && url_prefix[pl - 1] == '/')
        pl--;
    if (pl >= sizeof c->prefix)
        return -1;
    memcpy(c->prefix, url_prefix, pl);
    c->prefix[pl] = '\0';
    c->prefix_len = pl;
    c->timeout_ms = timeout_ms > 0 ? timeout_ms : CGI_DEFAULT_TIMEOUT_MS;
    c->max_output = CGI_MAX_OUTPUT;
    return 0;
}

/*
 * 환경 변수 "K=V\0" 들을 store 에 쌓고 envp 포인터 배열을 만든다.
 * 서버 자신의 환경(비밀 값이 있을 수 있음)은 넘기지 않고 CGI 에 필요한 것만 명시적으로 만든다.
 */
static int build_env(buf_t *store, char *envp[CGI_ENV_MAX], const http_request_t *req,
                     const conn_info_t *ci, const char *script_name, size_t script_len)
{
    size_t offs[CGI_ENV_MAX];
    size_t n = 0;
    size_t i;
    size_t ct_len = 0;
    const char *ct = http_header_get(req, "Content-Type", &ct_len);

#define ENV_ADD(...)                                                                   \
    do {                                                                               \
        offs[n++] = store->len;                                                        \
        if (buf_appendf(store, __VA_ARGS__) != BUF_OK || buf_append(store, "", 1) != BUF_OK) \
            return -1;                                                                 \
    } while (0)

    ENV_ADD("GATEWAY_INTERFACE=CGI/1.1");
    ENV_ADD("SERVER_SOFTWARE=%s", HTTP_SERVER_NAME);
    ENV_ADD("SERVER_PROTOCOL=HTTP/1.%d", req->version_minor);
    ENV_ADD("REQUEST_METHOD=%s", http_method_name(req->method));
    ENV_ADD("SCRIPT_NAME=%.*s", (int)script_len, script_name);
    ENV_ADD("QUERY_STRING=%.*s", (int)req->query_len, req->query ? req->query : "");
    ENV_ADD("REMOTE_ADDR=%s", ci && ci->ip ? ci->ip : "-");
    ENV_ADD("PATH=/usr/local/bin:/usr/bin:/bin");
    if (req->method == HTTP_POST || req->content_length > 0)
        ENV_ADD("CONTENT_LENGTH=%zu", req->content_length);
    if (ct != NULL)
        ENV_ADD("CONTENT_TYPE=%.*s", (int)ct_len, ct);
#undef ENV_ADD

    /* store 가 realloc 될 수 있으므로 포인터는 다 쌓은 뒤에 계산한다 */
    for (i = 0; i < n; i++)
        envp[i] = store->data + offs[i];
    envp[n] = NULL;
    return 0;
}

static int reply_status(int fd, int status, int ka, int head, const char *extra,
                        handler_result_t *res)
{
    size_t sent = 0;
    int rc = http_response_status_page(fd, status, ka, head, extra, &sent);

    res->status = status;
    res->bytes = sent;
    return rc;
}

int cgi_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci, int keep_alive,
               handler_result_t *res)
{
    const cgi_ctx_t *c = ctx;
    const char *name;
    size_t nlen;
    char path[PATH_MAX];
    char *envp[CGI_ENV_MAX];
    struct stat st;
    buf_t env;
    buf_t out;
    buf_t head;
    buf_t hdrs;
    int is_head = req->method == HTTP_HEAD;
    int rc = 0;
    int status;
    int run;
    size_t body_off = 0;
    pid_t pid = -1;

    buf_init(&env, 1024, 64 * 1024);
    buf_init(&out, 0, c->max_output + 1);
    buf_init(&head, 0, CGI_MAX_HEADERS_BYTES + 4096 + c->max_output);
    buf_init(&hdrs, 0, CGI_MAX_HEADERS_BYTES);

    if (req->method != HTTP_GET && req->method != HTTP_HEAD && req->method != HTTP_POST) {
        rc = reply_status(fd, 405, keep_alive, is_head, "Allow: GET, HEAD, POST\r\n", res);
        goto cleanup;
    }
    /* "/cgi-bin/<name>" 에서 name 추출. 하위 경로(PATH_INFO)는 지원하지 않는다. */
    if (req->path_len <= c->prefix_len + 1 || req->path[c->prefix_len] != '/') {
        rc = reply_status(fd, 404, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    name = req->path + c->prefix_len + 1;
    nlen = req->path_len - c->prefix_len - 1;
    if (!cgi_script_name_ok(name, nlen)) {
        rc = reply_status(fd, 404, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    if ((size_t)snprintf(path, sizeof path, "%s/%.*s", c->dir, (int)nlen, name) >= sizeof path) {
        rc = reply_status(fd, 414, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    if (stat(path, &st) != 0) {
        rc = reply_status(fd, errno == ENOENT ? 404 : 403, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    if (!S_ISREG(st.st_mode) || access(path, X_OK) != 0) {
        rc = reply_status(fd, 403, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    if (build_env(&env, envp, req, ci, req->path, req->path_len) != 0) {
        rc = reply_status(fd, 500, keep_alive, is_head, NULL, res);
        goto cleanup;
    }

    run = cgi_run(path, envp, req->body, req->body_len, c->timeout_ms, c->max_output, &out, &pid);
    if (run != CGI_OK) {
        static const char *const why[] = {"", "내부 오류", "타임아웃", "비정상 종료", "실행 실패",
                                          "출력 상한 초과"};

        log_msg(LOG_WARN, "CGI %s (pid %ld): %s", path, (long)pid, why[-run]);
        rc = reply_status(fd, run == CGI_ERR_TIMEOUT ? 504 : 500, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    if (cgi_parse_output(out.data ? out.data : "", out.len, &status, &hdrs, &body_off) != 0) {
        log_msg(LOG_WARN, "CGI %s: 출력 헤더 형식 오류", path);
        rc = reply_status(fd, 500, keep_alive, is_head, NULL, res);
        goto cleanup;
    }

    /*
     * CGI 가 준 헤더(Content-Type 포함)는 extra 로 그대로, Content-Length 는 서버가 계산.
     * 헤더와 본문을 한 버퍼로 합쳐 send 한 번으로 보낸다: 작은 write 두 번은 Nagle + 지연 ACK
     * 때문에 두 번째 조각이 수십 ms 늦게 나갈 수 있다.
     */
    if (http_response_head(&head, status, NULL, out.len - body_off, keep_alive,
                           hdrs.len ? hdrs.data : NULL) != BUF_OK ||
        (!is_head && buf_append(&head, out.data + body_off, out.len - body_off) != BUF_OK)) {
        rc = reply_status(fd, 500, keep_alive, is_head, NULL, res);
        goto cleanup;
    }
    res->status = status;
    if (send_all(fd, head.data, head.len) < 0) {
        rc = -1;
        goto cleanup;
    }
    res->bytes = head.len;

cleanup:
    buf_free(&hdrs);
    buf_free(&head);
    buf_free(&out);
    buf_free(&env);
    return rc;
}
