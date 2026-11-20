/*
 * static.c — 정적 파일 응답. 루트 탈출 방어 설계는 static.h 참고.
 *
 * 동기화: static_ctx_t 는 시작 시 한 번 쓰고 이후 읽기만 하므로 락이 필요 없다.
 */
#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buf.h"
#include "http_response.h"
#include "mime.h"
#include "netio.h"

#define STATIC_IO_BUF (64 * 1024) /* 파일 → 소켓 전송 고정 버퍼 */

/* ------------------------------------------------------------------ */
/* 순수 함수                                                           */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int static_url_decode(const char *in, size_t n, char *out, size_t outsz)
{
    size_t i = 0;
    size_t w = 0;

    if (outsz == 0)
        return STATIC_ETOOLONG;
    while (i < n) {
        unsigned char c = (unsigned char)in[i];

        if (c == '%') {
            int hi, lo;

            if (n - i < 3)
                return STATIC_EBAD;
            hi = hexval(in[i + 1]);
            lo = hexval(in[i + 2]);
            if (hi < 0 || lo < 0)
                return STATIC_EBAD;
            c = (unsigned char)(hi * 16 + lo);
            if (c == '\0')
                return STATIC_EBAD;
            i += 3;
        } else {
            i++;
        }
        if (w + 1 >= outsz) /* NUL 자리 */
            return STATIC_ETOOLONG;
        out[w++] = (char)c;
    }
    out[w] = '\0';
    return (int)w;
}

int static_path_normalize(char *path)
{
    size_t n = strlen(path);
    size_t i = 0;
    size_t w = 0; /* 출력 길이. 출력은 항상 입력보다 앞에 있으므로 제자리 처리가 안전하다 */

    if (n == 0 || path[0] != '/')
        return -1;
    while (i < n) {
        size_t s;
        size_t len;

        while (i < n && path[i] == '/')
            i++;
        s = i;
        while (i < n && path[i] != '/')
            i++;
        len = i - s;
        if (len == 0 || (len == 1 && path[s] == '.'))
            continue;
        if (len == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (w == 0)
                return -1; /* 루트 위로 탈출 */
            while (w > 0 && path[w - 1] != '/')
                w--;
            w--; /* 그 '/' 까지 제거 */
            continue;
        }
        path[w++] = '/';
        memmove(path + w, path + s, len); /* 겹칠 수 있으므로 memcpy 가 아닌 memmove */
        w += len;
    }
    if (w == 0)
        path[w++] = '/';
    path[w] = '\0';
    return 0;
}

int static_path_within(const char *root, size_t root_len, const char *resolved)
{
    if (root_len == 1 && root[0] == '/')
        return resolved[0] == '/';
    if (strncmp(resolved, root, root_len) != 0)
        return 0;
    return resolved[root_len] == '\0' || resolved[root_len] == '/';
}

/* ------------------------------------------------------------------ */
/* 핸들러                                                              */
/* ------------------------------------------------------------------ */

int static_init(static_ctx_t *s, const char *root)
{
    struct stat st;

    if (realpath(root, s->root) == NULL)
        return -1;
    if (stat(s->root, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    s->root_len = strlen(s->root);
    return 0;
}

static int errno_to_status(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENAMETOOLONG:
        return 414;
    default:
        return 500;
    }
}

/*
 * 요청을 실제 파일 경로(real)로 해석한다. 반환: 200(real 에 파일 후보) 또는 응답할 상태 코드.
 * 301/405 처럼 추가 헤더가 필요하면 extra 에 쓴다.
 */
static int resolve(const static_ctx_t *s, const http_request_t *req, char real[PATH_MAX],
                   buf_t *extra)
{
    char dec[PATH_MAX];
    char full[PATH_MAX];
    struct stat st;
    int n;
    int had_slash;

    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        buf_appendf(extra, "Allow: GET, HEAD\r\n");
        return 405;
    }

    /* 1단계: 문자열 — 디코딩 후 정규화 (순서가 중요: "%2e%2e" 를 ".." 로 만든 뒤 판정) */
    n = static_url_decode(req->path, req->path_len, dec, sizeof dec);
    if (n == STATIC_EBAD)
        return 400;
    if (n == STATIC_ETOOLONG)
        return 414;
    had_slash = n > 0 && dec[n - 1] == '/';
    if (static_path_normalize(dec) != 0)
        return 403;
    if ((size_t)snprintf(full, sizeof full, "%s%s", s->root, dec) >= sizeof full)
        return 414;

    /* 2단계: 파일시스템 — 심볼릭 링크를 푼 실제 경로로 재검사 */
    if (realpath(full, real) == NULL)
        return errno_to_status(errno);
    if (!static_path_within(s->root, s->root_len, real))
        return 403;
    if (stat(real, &st) != 0)
        return errno_to_status(errno);

    if (S_ISDIR(st.st_mode)) {
        if (!had_slash) {
            /* "/docs" → "/docs/": 그래야 index.html 안의 상대 링크가 올바른 기준으로 풀린다.
             * Location 에는 원본(인코딩된) 경로를 쓴다. 파서가 CR/LF 를 막았으므로 헤더 주입 불가. */
            buf_appendf(extra, "Location: %.*s/%s%.*s\r\n", (int)req->path_len, req->path,
                        req->query ? "?" : "", (int)req->query_len,
                        req->query ? req->query : "");
            return 301;
        }
        if ((size_t)snprintf(full, sizeof full, "%s/index.html", real) >= sizeof full)
            return 414;
        if (realpath(full, real) == NULL)
            return errno == ENOENT ? 403 : errno_to_status(errno); /* 목록은 보여주지 않는다 */
        if (!static_path_within(s->root, s->root_len, real))
            return 403;
    } else if (had_slash) {
        return 404; /* "/a.txt/" — 파일 뒤의 슬래시 */
    }
    return 200;
}

static int send_status(int fd, int status, int ka, int head, const buf_t *extra,
                       handler_result_t *res)
{
    size_t sent = 0;
    int rc = http_response_status_page(fd, status, ka, head, extra->len ? extra->data : NULL, &sent);

    res->status = status;
    res->bytes = sent;
    return rc;
}

int static_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res)
{
    const static_ctx_t *s = ctx;
    char real[PATH_MAX];
    buf_t extra;
    buf_t head;
    char *io = NULL;
    int file = -1;
    int is_head = req->method == HTTP_HEAD;
    int status;
    int rc = 0;
    struct stat st;
    size_t remaining;

    (void)ci;
    buf_init(&extra, 0, 16 * 1024);
    buf_init(&head, 0, 4096);

    status = resolve(s, req, real, &extra);
    if (status == 200) {
        /* realpath 이후 마지막 구성요소가 링크로 바뀌는 경쟁(TOCTOU)을 O_NOFOLLOW 로 막는다.
         * O_CLOEXEC: 다른 워커가 동시에 CGI 를 fork/exec 해도 이 fd 가 CGI 로 새지 않게 */
        file = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (file < 0)
            status = errno_to_status(errno);
        else if (fstat(file, &st) != 0)
            status = 500;
        else if (!S_ISREG(st.st_mode)) /* FIFO, 장치 파일 등 */
            status = 403;
    }
    if (status != 200) {
        rc = send_status(fd, status, keep_alive, is_head, &extra, res);
        goto cleanup;
    }

    remaining = (size_t)st.st_size;
    if (http_response_head(&head, 200, mime_type_for_path(real), remaining, keep_alive, NULL) !=
        BUF_OK) {
        rc = send_status(fd, 500, keep_alive, is_head, &extra, res);
        goto cleanup;
    }
    res->status = 200;
    if (send_all(fd, head.data, head.len) < 0) {
        rc = -1;
        goto cleanup;
    }
    res->bytes = head.len;
    if (is_head)
        goto cleanup;

    /* 고정 버퍼로 read → send_all 반복: 파일 크기와 무관하게 메모리 사용이 일정하다. */
    io = malloc(STATIC_IO_BUF);
    if (io == NULL) {
        rc = -1; /* 헤더를 이미 보냈으므로 오류 페이지를 보낼 수 없다 → 연결 종료 */
        goto cleanup;
    }
    while (remaining > 0) {
        size_t want = remaining < STATIC_IO_BUF ? remaining : STATIC_IO_BUF;
        ssize_t r = read(file, io, want);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            /* 전송 중 파일이 줄어듦: Content-Length 를 지킬 수 없으니 연결을 끊어 알린다 */
            res->close = 1;
            break;
        }
        if (send_all(fd, io, (size_t)r) < 0) {
            rc = -1;
            break;
        }
        res->bytes += (size_t)r;
        remaining -= (size_t)r;
    }

cleanup:
    free(io);
    if (file >= 0)
        close(file);
    buf_free(&head);
    buf_free(&extra);
    return rc;
}
