#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "buffer.h"

#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_URI 8192
#define HTTP_MAX_HEADER_BYTES 16384
#define HTTP_MAX_METHOD 16

typedef enum {
    HTTP_GET,
    HTTP_HEAD,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_OPTIONS,
    HTTP_PATCH,
    HTTP_CONNECT,
    HTTP_TRACE,
    HTTP_UNKNOWN
} http_method_t;

typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_INCOMPLETE = 1,
    HTTP_PARSE_ERROR = -1
} http_parse_result_t;

typedef enum {
    TARGET_ORIGIN,    /* /path?query          */
    TARGET_ABSOLUTE,  /* http://host:port/... (forward proxy) */
    TARGET_AUTHORITY, /* host:port            (CONNECT)       */
    TARGET_ASTERISK   /* *                    (OPTIONS)       */
} http_target_form_t;

typedef struct {
    char *name;
    char *value;
} http_header_t;

typedef struct {
    http_method_t method;
    char method_str[HTTP_MAX_METHOD + 1];
    http_target_form_t form;
    char *target;      /* raw request-target as received */
    char *path;        /* percent-decoded, dot-segment-normalized path */
    char *query;       /* raw query string without '?', "" when absent */
    char *host;        /* authority from absolute/authority form, else NULL */
    int port;          /* port from absolute/authority form */
    int version_minor; /* HTTP/1.<minor>: 0 or 1 */
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    char *body;
    size_t body_len;
    long long content_length; /* -1 when absent */
    int chunked;
    int keep_alive;
    int expect_continue;
    char *head_copy_; /* owned storage the string fields point into */
} http_request_t;

typedef struct {
    int status;
    int version_minor;
    char *reason;
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    size_t head_len;          /* bytes up to and including the blank line */
    long long content_length; /* -1 when absent */
    int chunked;
    int connection_close;
    char *head_copy_;
} http_response_head_t;

/* Incremental scanner that finds where a chunked body ends while streaming. */
typedef struct {
    int state;
    unsigned long long remaining;
    int digits;
} chunk_scanner_t;

/*
 * Parse one request from buf[0..len). On HTTP_PARSE_OK, *consumed is the number of
 * bytes the request occupied (pipelined data may follow). On HTTP_PARSE_ERROR,
 * *status is the HTTP status to answer with. On HTTP_PARSE_INCOMPLETE, *status is
 * 100 when the header block is complete and the client asked for 100-continue.
 */
http_parse_result_t http_parse_request(const char *buf, size_t len, size_t max_body,
                                       http_request_t *req, size_t *consumed, int *status);
void http_request_free(http_request_t *req);

http_parse_result_t http_parse_response_head(const char *buf, size_t len,
                                             http_response_head_t *res);
void http_response_head_free(http_response_head_t *res);

const char *http_header_get(const http_header_t *headers, size_t count, const char *name);
const char *http_request_header(const http_request_t *req, const char *name);
int http_header_has_token(const char *value, const char *token);

http_method_t http_method_from_str(const char *s, size_t len);
const char *http_reason(int status);

int http_url_decode(const char *in, size_t len, char *out, size_t outcap);
int http_normalize_path(char *path);
int http_encode_path(buf_t *out, const char *path);

void chunk_scanner_init(chunk_scanner_t *s);
/* Returns bytes consumed from data; sets *done once the terminating chunk + trailers end.
 * When out is non-NULL the decoded payload is appended at out + *out_len.
 * Returns -1 on malformed input. */
long chunk_scanner_feed(chunk_scanner_t *s, const char *data, size_t len, char *out,
                        size_t *out_len, int *done);

void http_format_date(long long epoch, char *out, size_t cap);

#endif
