/*
 * http_response.c — 응답 헤더 직렬화와 HTML 상태 페이지.
 *
 * 동기화: 상태를 갖지 않는다. 날짜는 gmtime_r (스레드 안전 버전) 을 쓴다.
 */
#include "http_response.h"

#include <stdio.h>

#include "netio.h"

/* 시작 시 한 번만 쓰고 이후 읽기 전용 (http_response.h 참고) */
static const char *g_backend_name;

void http_response_set_backend_name(const char *name)
{
    g_backend_name = name;
}

const char *http_status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

void http_date(char out[HTTP_DATE_LEN], time_t t)
{
    static const char *const wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(out, HTTP_DATE_LEN, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[tm.tm_wday], tm.tm_mday,
             mo[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int http_response_head(buf_t *out, int status, const char *ctype, size_t content_length,
                       int keep_alive, const char *extra_headers)
{
    char date[HTTP_DATE_LEN];

    http_date(date, time(NULL));
    return buf_appendf(out,
                       "HTTP/1.1 %d %s\r\n"
                       "Date: %s\r\n"
                       "Server: " HTTP_SERVER_NAME "\r\n"
                       "%s%s%s"
                       "Content-Length: %zu\r\n"
                       "Connection: %s\r\n"
                       "%s%s%s"
                       "%s"
                       "\r\n",
                       status, http_status_reason(status), date, ctype ? "Content-Type: " : "",
                       ctype ? ctype : "", ctype ? "\r\n" : "", content_length,
                       keep_alive ? "keep-alive" : "close", g_backend_name ? "X-Backend: " : "",
                       g_backend_name ? g_backend_name : "", g_backend_name ? "\r\n" : "",
                       extra_headers ? extra_headers : "");
}

int http_response_build_status_page(buf_t *out, int status, int keep_alive, int head_only,
                                    const char *extra_headers)
{
    char body[512];
    int blen;
    int rc;
    const char *reason = http_status_reason(status);

    blen = snprintf(body, sizeof body,
                    "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>\n"
                    "<body><h1>%d %s</h1><hr><p>" HTTP_SERVER_NAME "</p></body></html>\n",
                    status, reason, status, reason);
    if (blen < 0 || (size_t)blen >= sizeof body)
        return BUF_ERR_FMT;
    /* HEAD 응답도 GET 과 같은 Content-Length 를 알려야 한다 (RFC 9110 9.3.2). */
    rc = http_response_head(out, status, "text/html; charset=utf-8", (size_t)blen, keep_alive,
                            extra_headers);
    if (rc != BUF_OK || head_only)
        return rc;
    return buf_append(out, body, (size_t)blen);
}

int http_response_status_page(int fd, int status, int keep_alive, int head_only,
                              const char *extra_headers, size_t *sent)
{
    buf_t b;
    int rc = -1;

    *sent = 0;
    if (buf_init(&b, 1024, 64 * 1024) != BUF_OK)
        goto cleanup;
    if (http_response_build_status_page(&b, status, keep_alive, head_only, extra_headers) != BUF_OK)
        goto cleanup;
    if (send_all(fd, b.data, b.len) < 0)
        goto cleanup;
    *sent = b.len;
    rc = 0;

cleanup:
    buf_free(&b);
    return rc;
}
