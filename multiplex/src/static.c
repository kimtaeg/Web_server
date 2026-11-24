/*
 * static.c — 정적 파일 응답 계획. 루트 탈출 방어 설계는 static.h 참고.
 *
 * threadpool/ 판과의 차이: 소켓에 직접 쓰지 않는다. 무엇을 보낼지(헤더 + 파일 fd + 길이)만
 * 정하고, 실제 전송은 이벤트 루프가 소켓이 쓰기 가능할 때 조금씩 한다.
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

void static_plan_init(static_plan_t *p)
{
    p->status = 0;
    buf_init(&p->head, 0, STATIC_HEAD_MAX);
    p->file_fd = -1;
    p->file_len = 0;
}

void static_plan_free(static_plan_t *p)
{
    if (p->file_fd >= 0)
        close(p->file_fd);
    p->file_fd = -1;
    p->file_len = 0;
    buf_free(&p->head);
}

/* 상태 페이지 응답 전체를 head 에 만든다. 파일은 보내지 않는다. */
static int plan_status(static_plan_t *p, int status, int ka, int is_head, const buf_t *extra)
{
    if (p->file_fd >= 0)
        close(p->file_fd);
    p->file_fd = -1;
    p->file_len = 0;
    p->status = status;
    buf_reset(&p->head);
    if (http_response_build_status_page(&p->head, status, ka, is_head,
                                        extra->len ? extra->data : NULL) == BUF_OK)
        return 0;
    /* extra 가 너무 커서 실패했을 수도 있으니 마지막으로 추가 헤더 없는 500 을 시도 */
    p->status = 500;
    buf_reset(&p->head);
    return http_response_build_status_page(&p->head, 500, 0, is_head, NULL) == BUF_OK ? 0 : -1;
}

int static_prepare(const static_ctx_t *s, const http_request_t *req, int keep_alive,
                   static_plan_t *p)
{
    char real[PATH_MAX];
    buf_t extra;
    int is_head = req->method == HTTP_HEAD;
    int status;
    int rc;
    struct stat st;

    buf_init(&extra, 0, 16 * 1024);
    status = resolve(s, req, real, &extra);
    if (status == 200) {
        /* realpath 이후 마지막 구성요소가 링크로 바뀌는 경쟁(TOCTOU)을 O_NOFOLLOW 로 막는다.
         * O_CLOEXEC: 이 구현은 exec 하지 않지만, 습관적으로 모든 fd 에 붙인다 (threadpool/ 과 동일). */
        p->file_fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (p->file_fd < 0)
            status = errno_to_status(errno);
        else if (fstat(p->file_fd, &st) != 0)
            status = 500;
        else if (!S_ISREG(st.st_mode)) /* FIFO, 장치 파일 등: FIFO 를 read 하면 루프 전체가 멈춘다 */
            status = 403;
    }
    if (status != 200) {
        rc = plan_status(p, status, keep_alive, is_head, &extra);
        goto cleanup;
    }

    buf_reset(&p->head);
    if (http_response_head(&p->head, 200, mime_type_for_path(real), (size_t)st.st_size, keep_alive,
                           NULL) != BUF_OK) {
        rc = plan_status(p, 500, keep_alive, is_head, &extra);
        goto cleanup;
    }
    p->status = 200;
    p->file_len = is_head ? 0 : (size_t)st.st_size;
    if (p->file_len == 0) { /* HEAD 또는 빈 파일: 보낼 본문이 없으니 fd 를 바로 닫는다 */
        close(p->file_fd);
        p->file_fd = -1;
    }
    rc = 0;

cleanup:
    buf_free(&extra);
    return rc;
}
