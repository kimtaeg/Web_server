/*
 * http_parser.h — I/O 를 하지 않는 증분 HTTP/1.1 요청 파서.
 *
 * [사용 패턴]
 *   http_parser_init(&p, &limits);
 *   loop:
 *     recv 한 바이트를 buf 에 append
 *     r = http_parse(&p, buf.data, buf.len, &req, &used);
 *     NEED_MORE → 계속 읽는다
 *     ERROR     → http_parser_error_status(&p) 로 응답하고 연결을 닫는다
 *     OK        → req 처리 → buf_consume(&buf, used) → http_parser_init 후 다음 요청
 *
 * [왜 "버퍼 전체 + 재개 위치" 방식인가]
 * 호출자는 매번 지금까지 쌓인 버퍼 전체를 넘기고, 파서는 어디까지 처리했는지(pos)와
 * 상태(요청줄/헤더/본문)를 기억했다가 새로 온 바이트부터 이어서 본다. 전체 비용 O(n).
 * 파서가 바이트를 자기 쪽에 복사하지 않으므로 malloc 이 없다.
 * 대신 위치를 포인터가 아닌 "오프셋"으로 저장한다: buf 가 realloc 되면 주소가 바뀌기 때문.
 * PARSE_OK 시점에만 data + offset 으로 req 의 포인터를 채운다.
 *
 * 전제: 호출 사이에 data[0 .. 이전 len) 의 내용은 바뀌지 않는다 (뒤에 덧붙이기만 한다).
 *       주소(data)는 바뀌어도 된다.
 *
 * [분할 무관성]
 * 오류 판정은 항상 "왼쪽에서 오른쪽으로" 같은 순서로 검사한다. 그래서 요청이 어떻게 쪼개져
 * 도착하든 같은 바이트에서 같은 오류가 난다. 아직 줄이 끝나지 않았어도 길이 상한(414, 431)은
 * 즉시 판정해서, 끝없는 요청줄/헤더로 메모리를 고갈시키는 공격을 막는다.
 *
 * 스레드 안전성: 없음. 파서 하나는 연결 하나(워커 하나)가 소유한다.
 */
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_MAX_HEADERS 100   /* 헤더 배열 크기 (limits.max_headers 의 절대 상한) */
#define HTTP_MAX_METHOD_LEN 16 /* 이보다 긴 메서드 토큰은 501 */

typedef enum { PARSE_OK, PARSE_NEED_MORE, PARSE_ERROR } parse_result_t;

typedef enum {
    HTTP_GET,
    HTTP_HEAD,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_OPTIONS,
    HTTP_PATCH
} http_method_t;

typedef struct {
    const char *name;
    size_t name_len;
    const char *value; /* 앞뒤 공백(OWS) 제거됨 */
    size_t value_len;
} http_header_t;

/* 모든 포인터는 http_parse 에 넘긴 data 를 가리킨다 → 호출자가 consume 하기 전까지만 유효. */
typedef struct {
    http_method_t method;
    const char *target; /* 원본 request-target */
    size_t target_len;
    const char *path; /* '?' 앞. 퍼센트 디코딩 안 함(static 이 담당). absolute-form 에서 경로가 없으면 "/" */
    size_t path_len;
    const char *query; /* '?' 뒤. 없으면 NULL, 0 */
    size_t query_len;
    int is_absolute;       /* "http://authority/..." 형태 (포워드 프록시 요청) */
    const char *authority; /* is_absolute 일 때 host[:port], 아니면 NULL */
    size_t authority_len;
    int version_minor; /* HTTP/1.x 의 x: 0 또는 1 */
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    size_t content_length; /* Content-Length 가 없으면 0 */
    int keep_alive;
    const char *body; /* content_length 바이트 */
    size_t body_len;
} http_request_t;

typedef struct {
    size_t max_uri;          /* request-target 길이 상한 → 414 */
    size_t max_header_bytes; /* 헤더 영역(요청줄 다음 ~ 빈 줄까지) 상한 → 431 */
    size_t max_headers;      /* 헤더 개수 상한 → 431 */
    size_t max_body;         /* Content-Length 상한 → 413 */
} http_limits_t;

#define HTTP_LIMITS_DEFAULT {8192, 16384, 100, 1024 * 1024}

/* 내부 상태. 필드에 직접 접근하지 말 것. */
typedef struct {
    size_t off;
    size_t len;
} http_span_t;

typedef struct {
    http_limits_t lim;
    int state;
    int error_status;
    size_t pos;          /* 다음에 검사할 바이트 */
    size_t line_start;   /* 현재 줄의 시작 */
    size_t blank_lines;  /* 요청 앞의 빈 줄 수 */
    size_t headers_start;
    size_t body_start;
    http_method_t method;
    http_span_t target;
    int version_minor;
    http_span_t hname[HTTP_MAX_HEADERS];
    http_span_t hvalue[HTTP_MAX_HEADERS];
    size_t header_count;
    int has_host;
    int has_cl;
    int has_te;
    size_t content_length;
    int conn_close;
    int conn_keep_alive;
} http_parser_t;

/* lim 이 NULL 이면 HTTP_LIMITS_DEFAULT. 다음 요청을 파싱하기 전에도 다시 호출한다. */
void http_parser_init(http_parser_t *p, const http_limits_t *lim);

/*
 * PARSE_OK        : req 가 채워지고 *consumed = 이 요청이 차지한 바이트 수(앞의 빈 줄 + 헤더 + 본문).
 *                   data[*consumed ..] 는 파이프라이닝된 다음 요청이다.
 * PARSE_NEED_MORE : 바이트가 더 필요하다. *consumed = 0.
 * PARSE_ERROR     : 이후 호출도 계속 ERROR. 상태 코드는 http_parser_error_status().
 */
parse_result_t http_parse(http_parser_t *p, const char *data, size_t len, http_request_t *req,
                          size_t *consumed);

/* PARSE_ERROR 일 때의 HTTP 상태 코드 (400/411/413/414/431/501/505). 아니면 0. */
int http_parser_error_status(const http_parser_t *p);

/* 이름 대소문자 무시. 첫 번째로 일치하는 헤더의 값(OWS 제거)을 돌려준다. 없으면 NULL. */
const char *http_header_get(const http_request_t *req, const char *name, size_t *value_len);

const char *http_method_name(http_method_t m);

#endif /* HTTP_PARSER_H */
