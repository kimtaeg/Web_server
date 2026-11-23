#include "http.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Request line + header section; bigger heads are answered with 431. */
#define HTTP_MAX_HEAD (HTTP_MAX_URI + HTTP_MAX_HEADER_BYTES)
#define HTTP_MAX_RESPONSE_HEAD 65536

static const char *const method_names[] = {
    [HTTP_GET] = "GET",         [HTTP_HEAD] = "HEAD",       [HTTP_POST] = "POST",
    [HTTP_PUT] = "PUT",         [HTTP_DELETE] = "DELETE",   [HTTP_OPTIONS] = "OPTIONS",
    [HTTP_PATCH] = "PATCH",     [HTTP_CONNECT] = "CONNECT", [HTTP_TRACE] = "TRACE",
};

http_method_t http_method_from_str(const char *s, size_t len) {
    for (int m = 0; m < HTTP_UNKNOWN; m++) {
        if (strlen(method_names[m]) == len && memcmp(method_names[m], s, len) == 0)
            return (http_method_t)m;
    }
    return HTTP_UNKNOWN;
}

const char *http_reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 508: return "Loop Detected";
    default: return "Unknown";
    }
}

static int is_tchar(unsigned char c) {
    return isalnum(c) || (c && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int all_tchars(const char *s, size_t len) {
    if (len == 0)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (!is_tchar((unsigned char)s[i]))
            return 0;
    return 1;
}

/* Index just past the blank line that ends the head, or 0 if not received yet. */
static size_t find_head_end(const char *buf, size_t len) {
    const char *p = buf, *end = buf + len;
    while ((p = memchr(p, '\n', (size_t)(end - p))) != NULL) {
        if (p + 1 < end && p[1] == '\n')
            return (size_t)(p + 2 - buf);
        if (p + 2 < end && p[1] == '\r' && p[2] == '\n')
            return (size_t)(p + 3 - buf);
        p++;
    }
    return 0;
}

/* Split the next line in place (LF or CRLF). Returns NULL on a bare CR. */
static char *next_line(char **cursor, int *bad) {
    char *line = *cursor;
    char *nl = strchr(line, '\n');
    if (!nl) {
        *cursor = line + strlen(line);
    } else {
        *nl = '\0';
        *cursor = nl + 1;
        if (nl > line && nl[-1] == '\r')
            nl[-1] = '\0';
    }
    if (strchr(line, '\r')) {
        *bad = 1;
        return NULL;
    }
    return line;
}

const char *http_header_get(const http_header_t *headers, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (strcasecmp(headers[i].name, name) == 0)
            return headers[i].value;
    return NULL;
}

const char *http_request_header(const http_request_t *req, const char *name) {
    return http_header_get(req->headers, req->header_count, name);
}

int http_header_has_token(const char *value, const char *token) {
    if (!value)
        return 0;
    size_t tlen = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) == tlen && strncasecmp(start, token, tlen) == 0)
            return 1;
    }
    return 0;
}

/* Parse header lines until the blank line. Returns 0 or an HTTP status on error. */
static int parse_header_lines(char **cursor, http_header_t *headers, size_t *count) {
    int bad = 0;
    *count = 0;
    for (;;) {
        char *line = next_line(cursor, &bad);
        if (bad)
            return 400;
        if (*line == '\0')
            return 0;
        if (*line == ' ' || *line == '\t') /* obsolete line folding (RFC 7230 3.2.4) */
            return 400;
        char *colon = strchr(line, ':');
        if (!colon || !all_tchars(line, (size_t)(colon - line))) /* also rejects "Name :" */
            return 400;
        if (*count >= HTTP_MAX_HEADERS)
            return 431;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *vend = value + strlen(value);
        while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t'))
            *--vend = '\0';
        for (const unsigned char *v = (const unsigned char *)value; *v; v++)
            if ((*v < 0x20 && *v != '\t') || *v == 0x7f)
                return 400;
        headers[*count].name = line;
        headers[*count].value = value;
        (*count)++;
    }
}

static long long parse_content_length(const char *v) {
    if (!*v)
        return -1;
    long long n = 0;
    for (const char *p = v; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return -1;
        if (n > (LLONG_MAX - 9) / 10)
            return -2;
        n = n * 10 + (*p - '0');
    }
    return n;
}

int http_url_decode(const char *in, size_t len, char *out, size_t outcap) {
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if (w + 1 >= outcap)
            return -1;
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= len)
                return -1;
            if (!isxdigit((unsigned char)in[i + 1]) || !isxdigit((unsigned char)in[i + 2]))
                return -1;
            char hex[3] = {in[i + 1], in[i + 2], 0};
            c = (unsigned char)strtol(hex, NULL, 16);
            if (c == 0)
                return -1;
            i += 2;
        }
        out[w++] = (char)c;
    }
    out[w] = '\0';
    return (int)w;
}

int http_normalize_path(char *p) {
    if (p[0] != '/')
        return -1;
    size_t i = 0, w = 0;
    int ends_as_dir = 0;
    while (p[i]) {
        while (p[i] == '/')
            i++;
        size_t start = i;
        while (p[i] && p[i] != '/')
            i++;
        size_t seglen = i - start;
        ends_as_dir = (p[i] == '/' || seglen == 0);
        if (seglen == 0)
            break;
        if (seglen == 1 && p[start] == '.') {
            ends_as_dir = 1;
            continue;
        }
        if (seglen == 2 && p[start] == '.' && p[start + 1] == '.') {
            if (w == 0)
                return -1; /* would climb above the root */
            while (w > 0 && p[w - 1] != '/')
                w--;
            if (w > 0)
                w--;
            ends_as_dir = 1;
            continue;
        }
        p[w++] = '/';
        memmove(p + w, p + start, seglen);
        w += seglen;
    }
    if (ends_as_dir || w == 0)
        p[w++] = '/';
    p[w] = '\0';
    return 0;
}

int http_encode_path(buf_t *out, const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("-._~!$&'()*+,;=:@/", *p)) {
            if (buf_append(out, p, 1) < 0)
                return -1;
        } else {
            char e[3] = {'%', hex[*p >> 4], hex[*p & 15]};
            if (buf_append(out, e, 3) < 0)
                return -1;
        }
    }
    return 0;
}

enum {
    CS_SIZE, CS_EXT, CS_SIZE_LF, CS_DATA, CS_DATA_CR, CS_DATA_LF,
    CS_TRAILER_START, CS_TRAILER_LINE, CS_FINAL_LF, CS_DONE
};

void chunk_scanner_init(chunk_scanner_t *s) {
    s->state = CS_SIZE;
    s->remaining = 0;
    s->digits = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

long chunk_scanner_feed(chunk_scanner_t *s, const char *data, size_t len, char *out,
                        size_t *out_len, int *done) {
    size_t i = 0;
    *done = (s->state == CS_DONE);
    while (i < len && s->state != CS_DONE) {
        char c = data[i];
        switch (s->state) {
        case CS_SIZE: {
            int h = hexval(c);
            if (h >= 0) {
                if (++s->digits > 15)
                    return -1;
                s->remaining = s->remaining * 16 + (unsigned)h;
                i++;
                break;
            }
            if (s->digits == 0)
                return -1;
            if (c == ';' || c == ' ' || c == '\t')
                s->state = CS_EXT;
            else if (c == '\r')
                s->state = CS_SIZE_LF;
            else if (c == '\n')
                s->state = s->remaining ? CS_DATA : CS_TRAILER_START;
            else
                return -1;
            i++;
            break;
        }
        case CS_EXT:
            if (c == '\r')
                s->state = CS_SIZE_LF;
            else if (c == '\n')
                s->state = s->remaining ? CS_DATA : CS_TRAILER_START;
            i++;
            break;
        case CS_SIZE_LF:
            if (c != '\n')
                return -1;
            s->state = s->remaining ? CS_DATA : CS_TRAILER_START;
            i++;
            break;
        case CS_DATA: {
            size_t n = len - i;
            if (n > s->remaining)
                n = (size_t)s->remaining;
            if (out) {
                memcpy(out + *out_len, data + i, n);
                *out_len += n;
            }
            s->remaining -= n;
            i += n;
            if (s->remaining == 0)
                s->state = CS_DATA_CR;
            break;
        }
        case CS_DATA_CR:
            if (c == '\r')
                s->state = CS_DATA_LF;
            else if (c == '\n')
                s->state = CS_SIZE, s->digits = 0;
            else
                return -1;
            i++;
            break;
        case CS_DATA_LF:
            if (c != '\n')
                return -1;
            s->state = CS_SIZE;
            s->digits = 0;
            i++;
            break;
        case CS_TRAILER_START:
            if (c == '\r')
                s->state = CS_FINAL_LF;
            else if (c == '\n')
                s->state = CS_DONE;
            else
                s->state = CS_TRAILER_LINE;
            i++;
            break;
        case CS_TRAILER_LINE:
            if (c == '\n')
                s->state = CS_TRAILER_START;
            i++;
            break;
        case CS_FINAL_LF:
            if (c != '\n')
                return -1;
            s->state = CS_DONE;
            i++;
            break;
        }
    }
    *done = (s->state == CS_DONE);
    return (long)i;
}

/* Fill host/port from "host[:port]" or "[v6]:port" into scratch. Returns 0 or -1. */
static int parse_authority(const char *a, size_t len, int default_port, char *host_out,
                           int *port_out) {
    const char *host = a, *colon = NULL;
    size_t hlen;
    if (len && a[0] == '[') {
        const char *rb = memchr(a, ']', len);
        if (!rb)
            return -1;
        host = a + 1;
        hlen = (size_t)(rb - host);
        if (rb + 1 < a + len) {
            if (rb[1] != ':')
                return -1;
            colon = rb + 1;
        }
    } else {
        colon = memchr(a, ':', len);
        hlen = colon ? (size_t)(colon - a) : len;
    }
    if (hlen == 0 || hlen > 253 || memchr(a, '@', len))
        return -1;
    for (size_t i = 0; i < hlen; i++)
        if (!(isalnum((unsigned char)host[i]) || strchr("-._:", host[i])))
            return -1;
    int port = default_port;
    if (colon) {
        const char *ps = colon + 1, *pe = a + len;
        if (ps == pe || pe - ps > 5)
            return -1;
        port = 0;
        for (const char *p = ps; p < pe; p++) {
            if (!isdigit((unsigned char)*p))
                return -1;
            port = port * 10 + (*p - '0');
        }
        if (port < 1 || port > 65535)
            return -1;
    }
    if (port <= 0)
        return -1;
    memcpy(host_out, host, hlen);
    host_out[hlen] = '\0';
    *port_out = port;
    return 0;
}

/* Split "/path?query#frag" into decoded+normalized path and raw query. */
static int parse_origin(const char *t, size_t tlen, char *path_out, size_t path_cap,
                        char *query_out) {
    const char *hash = memchr(t, '#', tlen);
    if (hash)
        tlen = (size_t)(hash - t);
    const char *q = memchr(t, '?', tlen);
    size_t plen = q ? (size_t)(q - t) : tlen;
    if (q) {
        size_t qlen = tlen - plen - 1;
        memcpy(query_out, q + 1, qlen);
        query_out[qlen] = '\0';
    } else {
        query_out[0] = '\0';
    }
    if (plen == 0) {
        strcpy(path_out, "/");
        return 0;
    }
    if (http_url_decode(t, plen, path_out, path_cap) < 0 || path_out[0] != '/')
        return 400;
    if (http_normalize_path(path_out) < 0)
        return 403;
    return 0;
}

static int parse_target(http_request_t *req, char *scratch, size_t tlen) {
    const char *t = req->target;
    char *host_out = scratch;
    char *query_out = scratch + tlen + 2;
    char *path_out = scratch + 2 * (tlen + 2);
    size_t path_cap = tlen + 2;

    req->query = query_out;
    req->path = path_out;
    query_out[0] = '\0';

    if (req->method == HTTP_CONNECT) {
        req->form = TARGET_AUTHORITY;
        if (parse_authority(t, tlen, 0, host_out, &req->port) < 0)
            return 400;
        req->host = host_out;
        strcpy(path_out, "/");
        return 0;
    }
    if (t[0] == '/') {
        req->form = TARGET_ORIGIN;
        return parse_origin(t, tlen, path_out, path_cap, query_out);
    }
    if (tlen == 1 && t[0] == '*') {
        if (req->method != HTTP_OPTIONS)
            return 400;
        req->form = TARGET_ASTERISK;
        strcpy(path_out, "*");
        return 0;
    }
    if (tlen > 7 && strncasecmp(t, "http://", 7) == 0) {
        req->form = TARGET_ABSOLUTE;
        const char *auth = t + 7;
        size_t alen = strcspn(auth, "/?#");
        if (parse_authority(auth, alen, 80, host_out, &req->port) < 0)
            return 400;
        req->host = host_out;
        return parse_origin(auth + alen, tlen - 7 - alen, path_out, path_cap, query_out);
    }
    return 400;
}

static int parse_request_line(http_request_t *req, char *line) {
    char *sp1 = strchr(line, ' ');
    if (!sp1)
        return 400;
    size_t mlen = (size_t)(sp1 - line);
    if (!all_tchars(line, mlen))
        return 400;
    if (mlen > HTTP_MAX_METHOD)
        return 501;
    memcpy(req->method_str, line, mlen);
    req->method_str[mlen] = '\0';
    req->method = http_method_from_str(line, mlen);

    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2 || sp2 == target)
        return 400; /* HTTP/0.9 style request or double space */
    *sp2 = '\0';
    if (sp2 - target > HTTP_MAX_URI)
        return 414;
    for (const unsigned char *p = (const unsigned char *)target; *p; p++)
        if (*p <= 0x20 || *p >= 0x7f)
            return 400;
    req->target = target;

    const char *v = sp2 + 1;
    if (strlen(v) != 8 || strncmp(v, "HTTP/", 5) != 0 || !isdigit((unsigned char)v[5]) ||
        v[6] != '.' || !isdigit((unsigned char)v[7]))
        return 400;
    if (v[5] != '1')
        return 505;
    req->version_minor = v[7] == '0' ? 0 : 1;
    if (req->method == HTTP_UNKNOWN)
        return 501;
    return 0;
}

/* Header semantics: Host, framing, Connection, Expect. Returns 0 or a status. */
static int apply_headers(http_request_t *req) {
    int host_count = 0;
    long long cl = -1;
    const char *te = NULL;
    for (size_t i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];
        if (strcasecmp(h->name, "Host") == 0) {
            host_count++;
        } else if (strcasecmp(h->name, "Content-Length") == 0) {
            long long n = parse_content_length(h->value);
            if (n == -2)
                return 413;
            if (n < 0 || (cl >= 0 && n != cl))
                return 400;
            cl = n;
        } else if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            if (te)
                return 400;
            te = h->value;
        } else if (strcasecmp(h->name, "Expect") == 0) {
            if (strcasecmp(h->value, "100-continue") != 0)
                return 417;
            req->expect_continue = 1;
        }
    }
    if (host_count > 1 || (req->version_minor == 1 && host_count == 0))
        return 400;
    if (te) {
        /* A message with both is a classic request-smuggling vector (RFC 7230 3.3.3). */
        if (cl >= 0 || req->version_minor == 0)
            return 400;
        const char *last = strrchr(te, ',');
        last = last ? last + 1 : te;
        while (*last == ' ' || *last == '\t')
            last++;
        if (strcasecmp(last, "chunked") != 0)
            return 501;
        req->chunked = 1;
    }
    req->content_length = cl;
    const char *conn = http_request_header(req, "Connection");
    if (req->version_minor == 1)
        req->keep_alive = !http_header_has_token(conn, "close");
    else
        req->keep_alive = http_header_has_token(conn, "keep-alive");
    return 0;
}

static http_parse_result_t fail(http_request_t *req, int *status, int code) {
    http_request_free(req);
    *status = code;
    return HTTP_PARSE_ERROR;
}

http_parse_result_t http_parse_request(const char *buf, size_t len, size_t max_body,
                                       http_request_t *req, size_t *consumed, int *status) {
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
    *status = 0;
    *consumed = 0;

    size_t skip = 0; /* RFC 7230 3.5: ignore empty lines before the request-line */
    while (skip < len && skip < 8 && (buf[skip] == '\r' || buf[skip] == '\n'))
        skip++;
    buf += skip;
    len -= skip;

    size_t head_len = find_head_end(buf, len);
    if (head_len == 0) {
        const char *nl = memchr(buf, '\n', len);
        if (!nl && len > HTTP_MAX_URI + HTTP_MAX_METHOD + 16)
            return fail(req, status, 414);
        if (len > HTTP_MAX_HEAD)
            return fail(req, status, 431);
        return HTTP_PARSE_INCOMPLETE;
    }
    if (head_len > HTTP_MAX_HEAD)
        return fail(req, status, 431);

    /* One allocation: header copy, then scratch for host, query and decoded path. */
    const char *first_sp = memchr(buf, ' ', head_len);
    size_t tlen_max = head_len;
    if (first_sp)
        tlen_max = head_len - (size_t)(first_sp - buf);
    size_t alloc = head_len + 1 + 3 * (tlen_max + 2);
    req->head_copy_ = malloc(alloc);
    if (!req->head_copy_)
        return fail(req, status, 500);
    memcpy(req->head_copy_, buf, head_len);
    req->head_copy_[head_len] = '\0';
    if (memchr(req->head_copy_, '\0', head_len))
        return fail(req, status, 400);

    char *cursor = req->head_copy_;
    int bad = 0;
    char *line = next_line(&cursor, &bad);
    if (bad)
        return fail(req, status, 400);
    int rc = parse_request_line(req, line);
    if (rc)
        return fail(req, status, rc);
    rc = parse_header_lines(&cursor, req->headers, &req->header_count);
    if (rc)
        return fail(req, status, rc);
    rc = apply_headers(req);
    if (rc)
        return fail(req, status, rc);
    rc = parse_target(req, req->head_copy_ + head_len + 1, strlen(req->target));
    if (rc)
        return fail(req, status, rc);

    const char *body = buf + head_len;
    size_t avail = len - head_len;
    if (req->chunked) {
        req->body = malloc(avail + 1);
        if (!req->body)
            return fail(req, status, 500);
        chunk_scanner_t sc;
        chunk_scanner_init(&sc);
        size_t out_len = 0;
        int done = 0;
        long used = chunk_scanner_feed(&sc, body, avail, req->body, &out_len, &done);
        if (used < 0)
            return fail(req, status, 400);
        if (out_len > max_body || out_len + sc.remaining > max_body)
            return fail(req, status, 413);
        if (!done) {
            free(req->body);
            req->body = NULL;
            *status = req->expect_continue ? 100 : 0;
            http_request_free(req);
            return HTTP_PARSE_INCOMPLETE;
        }
        req->body[out_len] = '\0';
        req->body_len = out_len;
        *consumed = skip + head_len + (size_t)used;
        return HTTP_PARSE_OK;
    }

    size_t cl = req->content_length > 0 ? (size_t)req->content_length : 0;
    if (cl > max_body)
        return fail(req, status, 413);
    if (avail < cl) {
        *status = req->expect_continue ? 100 : 0;
        http_request_free(req);
        return HTTP_PARSE_INCOMPLETE;
    }
    if (cl) {
        req->body = malloc(cl + 1);
        if (!req->body)
            return fail(req, status, 500);
        memcpy(req->body, body, cl);
        req->body[cl] = '\0';
        req->body_len = cl;
    }
    *consumed = skip + head_len + cl;
    return HTTP_PARSE_OK;
}

void http_request_free(http_request_t *req) {
    free(req->head_copy_);
    free(req->body);
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
}

http_parse_result_t http_parse_response_head(const char *buf, size_t len,
                                             http_response_head_t *res) {
    memset(res, 0, sizeof(*res));
    res->content_length = -1;
    size_t head_len = find_head_end(buf, len);
    if (head_len == 0)
        return len > HTTP_MAX_RESPONSE_HEAD ? HTTP_PARSE_ERROR : HTTP_PARSE_INCOMPLETE;
    res->head_copy_ = malloc(head_len + 1);
    if (!res->head_copy_)
        return HTTP_PARSE_ERROR;
    memcpy(res->head_copy_, buf, head_len);
    res->head_copy_[head_len] = '\0';
    res->head_len = head_len;

    char *cursor = res->head_copy_;
    int bad = 0;
    char *line = next_line(&cursor, &bad);
    if (bad || strncmp(line, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)line[7]) ||
        line[8] != ' ' || !isdigit((unsigned char)line[9]) ||
        !isdigit((unsigned char)line[10]) || !isdigit((unsigned char)line[11]) ||
        (line[12] != ' ' && line[12] != '\0'))
        goto bad;
    res->version_minor = line[7] - '0';
    res->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    res->reason = line[12] ? line + 13 : line + 12;
    if (res->status < 100)
        goto bad;
    if (parse_header_lines(&cursor, res->headers, &res->header_count) != 0)
        goto bad;
    for (size_t i = 0; i < res->header_count; i++) {
        const http_header_t *h = &res->headers[i];
        if (strcasecmp(h->name, "Content-Length") == 0) {
            long long n = parse_content_length(h->value);
            if (n < 0 || (res->content_length >= 0 && n != res->content_length))
                goto bad;
            res->content_length = n;
        } else if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            res->chunked = http_header_has_token(h->value, "chunked");
        } else if (strcasecmp(h->name, "Connection") == 0) {
            res->connection_close = http_header_has_token(h->value, "close");
        }
    }
    if (res->chunked)
        res->content_length = -1; /* RFC 7230 3.3.3: Transfer-Encoding wins */
    return HTTP_PARSE_OK;
bad:
    http_response_head_free(res);
    return HTTP_PARSE_ERROR;
}

void http_response_head_free(http_response_head_t *res) {
    free(res->head_copy_);
    memset(res, 0, sizeof(*res));
}

void http_format_date(long long epoch, char *out, size_t cap) {
    time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}
