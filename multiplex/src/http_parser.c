/*
 * http_parser.c — I/O 없는 증분 HTTP/1.1 요청 파서. 설계 개요는 http_parser.h 참고.
 *
 * 상태 전이:
 *   START ──(빈 줄 건너뜀)──▶ REQLINE ──▶ HEADERS ──(빈 줄)──▶ BODY ──▶ DONE
 *     어느 상태에서든 오류 ──▶ ERROR (이후 계속 ERROR)
 *
 * [분할 무관성을 지키는 규칙]
 * 1. 완성된 줄은 도착하는 즉시 순서대로 처리한다. 앞줄의 오류가 항상 먼저 보고된다.
 * 2. 한 줄 안에서는 scan_reqline 하나가 왼쪽→오른쪽으로 검사한다. 줄이 아직 안 끝났을 때(prefix)도
 *    같은 함수를 쓰고, 끝에 다다르면 "더 필요"로 멈출 뿐이라 판정이 어긋나지 않는다.
 * 3. 줄 끝(LF 앞에 CR 이 있는가)은 줄의 가장 오른쪽이므로 가장 마지막에 검사한다.
 */
#include "http_parser.h"

#include <stdint.h>
#include <string.h>
#include <strings.h> /* strncasecmp (POSIX) */

enum { S_START, S_REQLINE, S_HEADERS, S_BODY, S_DONE, S_ERROR };

#define MAX_BLANK_LINES 8 /* RFC 9112 2.2: 요청 앞의 빈 줄은 무시하되, 무한정 받지는 않는다 */
#define SCAN_NEED_MORE (-1)

static const struct {
    const char *name;
    size_t len;
    http_method_t method;
} methods[] = {
    {"GET", 3, HTTP_GET},       {"HEAD", 4, HTTP_HEAD},       {"POST", 4, HTTP_POST},
    {"PUT", 3, HTTP_PUT},       {"DELETE", 6, HTTP_DELETE},   {"OPTIONS", 7, HTTP_OPTIONS},
    {"PATCH", 5, HTTP_PATCH},
};
#define N_METHODS (sizeof methods / sizeof methods[0])

/* ------------------------------------------------------------------ */
/* 문자 분류 (RFC 9110 5.6.2)                                          */
/* ------------------------------------------------------------------ */

static int is_tchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* request-target: 출력 가능한 ASCII 만 (공백·제어문자·비ASCII 금지) */
static int is_target_char(unsigned char c)
{
    return c > 0x20 && c < 0x7f;
}

/* field-value: HTAB, SP, VCHAR, obs-text(0x80~) 허용. 그 밖의 제어문자(CR, NUL, DEL 등) 금지. */
static int is_value_char(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}

static int is_ows(char c)
{
    return c == ' ' || c == '\t';
}

static int span_ieq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && strncasecmp(s, lit, n) == 0;
}

static parse_result_t fail(http_parser_t *p, int status)
{
    p->state = S_ERROR;
    p->error_status = status;
    return PARSE_ERROR;
}

/* ------------------------------------------------------------------ */
/* 요청줄                                                              */
/* ------------------------------------------------------------------ */

/* "http://" → 7, "https://" → 8, 그 외 0 (대소문자 무시). https 는 형식만 허용하고 처리는 fproxy 가 거절. */
static size_t scheme_len(const char *t, size_t n)
{
    if (n >= 7 && strncasecmp(t, "http://", 7) == 0)
        return 7;
    if (n >= 8 && strncasecmp(t, "https://", 8) == 0)
        return 8;
    return 0;
}

/* 대상 형태 검사: origin-form("/..."), absolute-form("http(s)://authority..."), asterisk("*", OPTIONS 만). */
static int check_target_form(http_method_t m, const char *t, size_t n)
{
    size_t a;

    if (t[0] == '/')
        return 0;
    if (t[0] == '*')
        return (n == 1 && m == HTTP_OPTIONS) ? 0 : 400;
    {
        size_t sl = scheme_len(t, n);

        if (sl > 0) {
            for (a = sl; a < n && t[a] != '/' && t[a] != '?'; a++)
                ;
            return a > sl ? 0 : 400; /* authority 가 비면 안 된다 */
        }
    }
    return 400;
}

/*
 * 요청줄 s[0..n) 을 왼쪽에서 오른쪽으로 검사한다.
 * complete=0 이면 줄이 아직 끝나지 않은 것: 끝에 다다르면 SCAN_NEED_MORE.
 * 반환: 0(정상, complete 일 때만 필드 기록) / SCAN_NEED_MORE / HTTP 상태 코드.
 * base = s 의 버퍼 내 오프셋 (필드를 오프셋으로 기록하기 위해).
 */
static int scan_reqline(http_parser_t *p, const char *s, size_t n, size_t base, int complete)
{
    static const char vpfx[] = "HTTP/";
    size_t i = 0;
    size_t k;
    size_t tstart;
    size_t tlen;
    size_t vstart;
    size_t vlen;
    int found = 0;
    int st;

    /* method = token */
    while (i < n && s[i] != ' ') {
        if (i >= HTTP_MAX_METHOD_LEN)
            return 501;
        if (!is_tchar((unsigned char)s[i]))
            return 400;
        i++;
    }
    if (i == n)
        return complete ? 400 : SCAN_NEED_MORE;
    if (i == 0)
        return 400;
    for (k = 0; k < N_METHODS; k++) {
        if (methods[k].len == i && memcmp(s, methods[k].name, i) == 0) { /* 메서드는 대소문자 구분 */
            p->method = methods[k].method;
            found = 1;
            break;
        }
    }
    if (!found)
        return 501;

    /* request-target */
    i++;
    tstart = i;
    while (i < n && s[i] != ' ') {
        if (i - tstart >= p->lim.max_uri) /* 길이를 문자 검사보다 먼저: 분할 무관성 */
            return 414;
        if (!is_target_char((unsigned char)s[i]))
            return 400;
        i++;
    }
    if (i == n)
        return complete ? 400 : SCAN_NEED_MORE;
    tlen = i - tstart;
    if (tlen == 0)
        return 400;
    st = check_target_form(p->method, s + tstart, tlen);
    if (st != 0)
        return st;

    /* HTTP-version = "HTTP/" DIGIT "." DIGIT (정확히 8바이트) */
    i++;
    vstart = i;
    vlen = n - vstart;
    for (k = 0; k < vlen; k++) {
        unsigned char c = (unsigned char)s[vstart + k];
        int ok;

        if (k < 5)
            ok = c == (unsigned char)vpfx[k];
        else if (k == 5 || k == 7)
            ok = c >= '0' && c <= '9';
        else if (k == 6)
            ok = c == '.';
        else
            ok = 0; /* 8바이트 초과 */
        if (!ok)
            return 400;
    }
    if (vlen < 8)
        return complete ? 400 : SCAN_NEED_MORE;
    if (s[vstart + 5] != '1' || s[vstart + 7] > '1')
        return 505;

    p->target.off = base + tstart;
    p->target.len = tlen;
    p->version_minor = s[vstart + 7] - '0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* 헤더                                                                */
/* ------------------------------------------------------------------ */

/* Content-Length = 1*DIGIT. 부호, 공백, 목록("5, 5")은 거부. overflow 검사. */
static int parse_content_length(const char *s, size_t n, size_t *out)
{
    size_t v = 0;
    size_t i;

    if (n == 0)
        return -1;
    for (i = 0; i < n; i++) {
        size_t d;

        if (s[i] < '0' || s[i] > '9')
            return -1;
        d = (size_t)(s[i] - '0');
        if (v > (SIZE_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* Connection: 쉼표로 구분된 토큰 목록. close / keep-alive 만 의미가 있다. */
static void parse_connection(http_parser_t *p, const char *s, size_t n)
{
    size_t i = 0;

    while (i < n) {
        size_t a;
        size_t b;

        while (i < n && (is_ows(s[i]) || s[i] == ','))
            i++;
        a = i;
        while (i < n && s[i] != ',')
            i++;
        b = i;
        while (b > a && is_ows(s[b - 1]))
            b--;
        if (span_ieq(s + a, b - a, "close"))
            p->conn_close = 1;
        else if (span_ieq(s + a, b - a, "keep-alive"))
            p->conn_keep_alive = 1;
    }
}

/* 헤더 한 줄 data[ls..le) (CRLF 제외) 를 처리한다. 반환: 0 또는 상태 코드. */
static int on_header_line(http_parser_t *p, const char *data, size_t ls, size_t le)
{
    const char *line = data + ls;
    size_t n = le - ls;
    size_t colon;
    size_t vs;
    size_t ve;
    size_t i;
    const char *val;
    size_t vlen;

    if (is_ows(line[0])) /* obs-fold: RFC 9112 5.2 는 거부를 허용. 스머글링 여지를 없앤다. */
        return 400;
    if (p->header_count >= p->lim.max_headers)
        return 431;

    for (colon = 0; colon < n && line[colon] != ':'; colon++)
        if (!is_tchar((unsigned char)line[colon])) /* "Host : a" 의 공백도 여기서 걸린다 */
            return 400;
    if (colon == n || colon == 0)
        return 400;

    vs = colon + 1;
    ve = n;
    while (vs < ve && is_ows(line[vs]))
        vs++;
    while (ve > vs && is_ows(line[ve - 1]))
        ve--;
    for (i = vs; i < ve; i++)
        if (!is_value_char((unsigned char)line[i]))
            return 400;

    val = line + vs;
    vlen = ve - vs;
    if (span_ieq(line, colon, "host")) {
        if (p->has_host)
            return 400;
        p->has_host = 1;
    } else if (span_ieq(line, colon, "content-length")) {
        size_t v;

        if (parse_content_length(val, vlen, &v) != 0)
            return 400;
        if (p->has_cl && v != p->content_length) /* 값이 같은 중복은 허용 (RFC 9110 8.6) */
            return 400;
        p->has_cl = 1;
        p->content_length = v;
    } else if (span_ieq(line, colon, "transfer-encoding")) {
        p->has_te = 1;
    } else if (span_ieq(line, colon, "connection")) {
        parse_connection(p, val, vlen);
    }

    p->hname[p->header_count].off = ls;
    p->hname[p->header_count].len = colon;
    p->hvalue[p->header_count].off = ls + vs;
    p->hvalue[p->header_count].len = vlen;
    p->header_count++;
    return 0;
}

/* 빈 줄을 만난 뒤 헤더 전체에 대한 판정. 순서가 곧 우선순위다. */
static int on_headers_end(http_parser_t *p)
{
    /*
     * CL 과 TE 가 함께 오면 프록시와 백엔드가 본문 경계를 다르게 해석할 수 있다(요청 스머글링).
     * 어느 쪽을 믿을지 고르지 않고 거부한다.
     */
    if (p->has_cl && p->has_te)
        return 400;
    if (p->version_minor == 1 && !p->has_host)
        return 400;
    /* chunked 요청 본문은 지원하지 않는다: 길이를 알려 달라는 411 (RFC 9110 15.5.12). */
    if (p->has_te)
        return 411;
    if (!p->has_cl &&
        (p->method == HTTP_POST || p->method == HTTP_PUT || p->method == HTTP_PATCH))
        return 411;
    /* 본문이 도착하기 전에 거부해서 큰 본문을 받느라 메모리를 쓰지 않게 한다. */
    if (p->content_length > p->lim.max_body)
        return 413;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 결과 조립                                                           */
/* ------------------------------------------------------------------ */

static void fill_request(const http_parser_t *p, const char *data, http_request_t *req)
{
    const char *t = data + p->target.off;
    size_t tl = p->target.len;
    const char *rest = t;
    size_t rl = tl;
    const char *q;
    size_t plen;
    size_t i;

    req->method = p->method;
    req->target = t;
    req->target_len = tl;
    req->is_absolute = 0;
    req->authority = NULL;
    req->authority_len = 0;

    req->is_https = 0;
    if (scheme_len(t, tl) > 0) {
        size_t sl = scheme_len(t, tl);
        size_t ae = sl;

        while (ae < tl && t[ae] != '/' && t[ae] != '?')
            ae++;
        req->is_absolute = 1;
        req->is_https = sl == 8;
        req->authority = t + sl;
        req->authority_len = ae - sl;
        rest = t + ae;
        rl = tl - ae;
    }

    q = rl > 0 ? memchr(rest, '?', rl) : NULL;
    plen = q ? (size_t)(q - rest) : rl;
    if (plen == 0) { /* "http://host" 또는 "http://host?x" → 경로는 "/" */
        req->path = "/";
        req->path_len = 1;
    } else {
        req->path = rest;
        req->path_len = plen;
    }
    if (q) {
        req->query = q + 1;
        req->query_len = (size_t)(rest + rl - (q + 1));
    } else {
        req->query = NULL;
        req->query_len = 0;
    }

    req->version_minor = p->version_minor;
    req->header_count = p->header_count;
    for (i = 0; i < p->header_count; i++) {
        req->headers[i].name = data + p->hname[i].off;
        req->headers[i].name_len = p->hname[i].len;
        req->headers[i].value = data + p->hvalue[i].off;
        req->headers[i].value_len = p->hvalue[i].len;
    }
    req->content_length = p->content_length;
    /* HTTP/1.1 은 기본 유지, HTTP/1.0 은 명시적 keep-alive 일 때만. close 는 항상 우선. */
    if (p->version_minor == 1)
        req->keep_alive = !p->conn_close;
    else
        req->keep_alive = p->conn_keep_alive && !p->conn_close;
    req->body = data + p->body_start;
    req->body_len = p->content_length;
}

/* ------------------------------------------------------------------ */
/* 공개 API                                                            */
/* ------------------------------------------------------------------ */

void http_parser_init(http_parser_t *p, const http_limits_t *lim)
{
    static const http_limits_t def = HTTP_LIMITS_DEFAULT;

    memset(p, 0, sizeof *p);
    p->lim = lim ? *lim : def;
    if (p->lim.max_headers > HTTP_MAX_HEADERS) /* 배열 밖에 쓰지 않도록 */
        p->lim.max_headers = HTTP_MAX_HEADERS;
    p->state = S_START;
}

parse_result_t http_parse(http_parser_t *p, const char *data, size_t len, http_request_t *req,
                          size_t *consumed)
{
    const char *nl;
    size_t e;
    size_t ls;
    int st;

    *consumed = 0;
    if (p->state == S_ERROR)
        return PARSE_ERROR;
    if (len < p->pos) /* 호출 규약 위반(버퍼가 줄어듦) → 서버 버그이므로 500 */
        return fail(p, 500);

    for (;;) {
        switch (p->state) {
        case S_START:
            while (p->pos < len) {
                char c = data[p->pos];

                if (c == '\n')
                    return fail(p, 400);
                if (c != '\r')
                    break;
                if (p->pos + 1 >= len)
                    return PARSE_NEED_MORE;
                if (data[p->pos + 1] != '\n')
                    return fail(p, 400);
                if (++p->blank_lines > MAX_BLANK_LINES)
                    return fail(p, 400);
                p->pos += 2;
            }
            if (p->pos >= len)
                return PARSE_NEED_MORE;
            p->line_start = p->pos;
            p->state = S_REQLINE;
            break;

        case S_REQLINE:
            ls = p->line_start;
            nl = memchr(data + p->pos, '\n', len - p->pos);
            if (nl == NULL) {
                size_t avail = len - ls;

                p->pos = len; /* 다음엔 새 바이트부터 '\n' 을 찾는다 */
                if (avail > 0 && data[len - 1] == '\r')
                    avail--; /* CRLF 의 CR 일 수 있으니 판정에서 뺀다 */
                /* 상한을 넘었을 때만 미완성 줄을 검사 → 끝없는 요청줄을 414/400/501 로 즉시 끊는다 */
                if (avail > p->lim.max_uri) {
                    st = scan_reqline(p, data + ls, avail, ls, 0);
                    if (st > 0)
                        return fail(p, st);
                }
                return PARSE_NEED_MORE;
            }
            e = (size_t)(nl - data);
            {
                int has_cr = e > ls && data[e - 1] == '\r';

                st = scan_reqline(p, data + ls, (has_cr ? e - 1 : e) - ls, ls, 1);
                if (st != 0)
                    return fail(p, st);
                if (!has_cr) /* bare LF: 줄의 가장 오른쪽이므로 마지막에 검사 */
                    return fail(p, 400);
            }
            p->pos = e + 1;
            p->line_start = p->pos;
            p->headers_start = p->pos;
            p->state = S_HEADERS;
            break;

        case S_HEADERS:
            for (;;) {
                nl = memchr(data + p->pos, '\n', len - p->pos);
                if (nl == NULL) {
                    p->pos = len;
                    if (len - p->headers_start > p->lim.max_header_bytes)
                        return fail(p, 431);
                    return PARSE_NEED_MORE;
                }
                e = (size_t)(nl - data);
                if (e + 1 - p->headers_start > p->lim.max_header_bytes)
                    return fail(p, 431);
                ls = p->line_start;
                if (e == ls || data[e - 1] != '\r')
                    return fail(p, 400);
                p->pos = e + 1;
                p->line_start = p->pos;
                if (e - 1 == ls) { /* 빈 줄 = 헤더 끝 */
                    st = on_headers_end(p);
                    if (st != 0)
                        return fail(p, st);
                    p->body_start = p->pos;
                    p->state = S_BODY;
                    break;
                }
                st = on_header_line(p, data, ls, e - 1);
                if (st != 0)
                    return fail(p, st);
            }
            break;

        case S_BODY:
            if (len - p->body_start < p->content_length) {
                p->pos = len;
                return PARSE_NEED_MORE;
            }
            p->state = S_DONE;
            break;

        case S_DONE:
            fill_request(p, data, req);
            *consumed = p->body_start + p->content_length;
            return PARSE_OK;

        default:
            return fail(p, 500);
        }
    }
}

int http_parser_headers_done(const http_parser_t *p)
{
    return p->state == S_BODY || p->state == S_DONE;
}

int http_parser_error_status(const http_parser_t *p)
{
    return p->state == S_ERROR ? p->error_status : 0;
}

const char *http_header_get(const http_request_t *req, const char *name, size_t *value_len)
{
    size_t n = strlen(name);
    size_t i;

    for (i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];

        if (h->name_len == n && strncasecmp(h->name, name, n) == 0) {
            if (value_len != NULL)
                *value_len = h->value_len;
            return h->value;
        }
    }
    return NULL;
}

const char *http_method_name(http_method_t m)
{
    size_t k;

    for (k = 0; k < N_METHODS; k++)
        if (methods[k].method == m)
            return methods[k].name;
    return "?";
}
