#include "proxy.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cache.h"
#include "log.h"
#include "netutil.h"
#include "response.h"

#define TUNNEL_IDLE_MS 30000

typedef enum {
    RELAY_OK = 0,
    RELAY_BAD_UPSTREAM,  /* closed or sent garbage before a response head -> 502 */
    RELAY_TIMEOUT,       /* no response head within proxy_timeout -> 504 */
    RELAY_BROKEN,        /* failed after the head was sent: the client sees a cut */
} relay_result_t;

static const char *const hop_headers[] = {
    "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Transfer-Encoding",
    "Upgrade", "Proxy-Authorization", "Proxy-Authenticate",
};

int proxy_is_hop_header(const char *name, const char *connection_value) {
    for (size_t i = 0; i < sizeof(hop_headers) / sizeof(hop_headers[0]); i++)
        if (strcasecmp(name, hop_headers[i]) == 0)
            return 1;
    return http_header_has_token(connection_value, name);
}

int proxy_build_request(buf_t *out, const http_request_t *req, const char *target,
                        const char *host_header, const char *client_ip) {
    const char *conn = http_request_header(req, "Connection");
    const char *xff = http_request_header(req, "X-Forwarded-For");
    const char *via = http_request_header(req, "Via");
    const char *orig_host = http_request_header(req, "Host");
    int rc = buf_printf(out, "%s %s HTTP/1.%d\r\nHost: %s\r\n", req->method_str, target,
                        req->version_minor, host_header);
    for (size_t i = 0; rc == 0 && i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        if (proxy_is_hop_header(name, conn) || !strcasecmp(name, "Host") ||
            !strcasecmp(name, "Content-Length") || !strcasecmp(name, "Expect") ||
            !strcasecmp(name, "X-Forwarded-For") || !strcasecmp(name, "X-Forwarded-Host") ||
            !strcasecmp(name, "X-Forwarded-Proto") || !strcasecmp(name, "Via"))
            continue;
        rc = buf_printf(out, "%s: %s\r\n", name, req->headers[i].value);
    }
    if (rc == 0)
        rc = buf_printf(out, "X-Forwarded-For: %s%s%s\r\nX-Forwarded-Proto: http\r\n",
                        xff ? xff : "", xff ? ", " : "", client_ip);
    if (rc == 0 && orig_host)
        rc = buf_printf(out, "X-Forwarded-Host: %s\r\n", orig_host);
    if (rc == 0)
        rc = buf_printf(out, "Via: %s%s1.%d %s\r\n", via ? via : "", via ? ", " : "",
                        req->version_minor, PROXY_VIA_TOKEN);
    if (rc == 0 && (req->body_len || req->method == HTTP_POST || req->method == HTTP_PUT ||
                    req->method == HTTP_PATCH))
        rc = buf_printf(out, "Content-Length: %zu\r\n", req->body_len);
    /* One request per upstream connection keeps framing simple and failures isolated. */
    if (rc == 0)
        rc = buf_append_str(out, "Connection: close\r\n\r\n");
    if (rc == 0 && req->body_len)
        rc = buf_append(out, req->body, req->body_len);
    return rc;
}

int proxy_response_head_lines(buf_t *out, const http_response_head_t *res) {
    const char *conn = http_header_get(res->headers, res->header_count, "Connection");
    int rc = buf_printf(out, "HTTP/1.1 %d %s\r\n", res->status,
                        res->reason && *res->reason ? res->reason : http_reason(res->status));
    for (size_t i = 0; rc == 0 && i < res->header_count; i++) {
        const char *name = res->headers[i].name;
        /* Transfer-Encoding stays: a chunked body is relayed byte-for-byte. */
        if (strcasecmp(name, "Transfer-Encoding") != 0 && proxy_is_hop_header(name, conn))
            continue;
        rc = buf_printf(out, "%s: %s\r\n", name, res->headers[i].value);
    }
    return rc;
}

typedef struct {
    buf_t *capture;   /* collects head lines + body for the cache, or NULL */
    size_t capture_limit;
    int capture_complete;
    int cache_ttl;    /* from Cache-Control; -1 = unspecified, 0 = do not store */
    const char *extra_headers;
} relay_opts_t;

static int capture(relay_opts_t *o, const char *data, size_t len) {
    if (!o->capture)
        return 0;
    if (o->capture->len + len > o->capture_limit || buf_append(o->capture, data, len) < 0) {
        o->capture = NULL; /* too big to cache; keep streaming */
        return -1;
    }
    return 0;
}

static int upstream_cacheable(const http_response_head_t *res) {
    if (res->status != 200 && res->status != 301 && res->status != 404)
        return 0;
    if (http_header_get(res->headers, res->header_count, "Set-Cookie"))
        return 0;
    const char *vary = http_header_get(res->headers, res->header_count, "Vary");
    return !(vary && strchr(vary, '*'));
}

/* Read the upstream response and stream it to the client. */
static relay_result_t relay_response(req_ctx_t *ctx, int upfd, relay_opts_t *o) {
    const config_t *cfg = ctx->srv->cfg;
    buf_t in;
    buf_init(&in);
    http_response_head_t res;
    char chunk[16384];
    relay_result_t result = RELAY_BAD_UPSTREAM;

    for (;;) {
        int w = wait_fd(upfd, POLLIN, cfg->proxy_timeout_ms);
        if (w == 0) {
            result = RELAY_TIMEOUT;
            goto out;
        }
        ssize_t n = w > 0 ? recv_some(upfd, chunk, sizeof(chunk)) : -1;
        if (n <= 0 || buf_append(&in, chunk, (size_t)n) < 0)
            goto out;
        http_parse_result_t pr = http_parse_response_head(in.data, in.len, &res);
        if (pr == HTTP_PARSE_ERROR)
            goto out;
        if (pr == HTTP_PARSE_INCOMPLETE)
            continue;
        if (res.status >= 100 && res.status < 200 && res.status != 101) {
            buf_consume(&in, res.head_len); /* interim response, e.g. 100 Continue */
            http_response_head_free(&res);
            if (in.len == 0)
                continue;
            pr = http_parse_response_head(in.data, in.len, &res);
            if (pr == HTTP_PARSE_ERROR)
                goto out;
            if (pr == HTTP_PARSE_INCOMPLETE)
                continue;
        }
        break;
    }

    int no_body = ctx->req->method == HTTP_HEAD || res.status == 204 || res.status == 304 ||
                  (res.status >= 100 && res.status < 200);
    long long remaining = no_body ? 0 : res.content_length;
    int chunked = !no_body && res.chunked;
    int until_eof = !no_body && !chunked && res.content_length < 0;
    if (until_eof)
        ctx->keep_alive = 0; /* body ends when the upstream closes: so must we */

    if (o->capture && !upstream_cacheable(&res))
        o->capture = NULL;
    if (o->capture) {
        o->cache_ttl = cache_ttl_from_cache_control(
            http_header_get(res.headers, res.header_count, "Cache-Control"));
        if (o->cache_ttl == 0)
            o->capture = NULL;
    }

    buf_t head;
    buf_init(&head);
    proxy_response_head_lines(&head, &res);
    capture(o, head.data, head.len);
    capture(o, "\r\n", 2);
    buf_printf(&head, "Connection: %s\r\n%s\r\n", ctx->keep_alive ? "keep-alive" : "close",
               o->extra_headers ? o->extra_headers : "");
    ctx->status = res.status;
    int sent = ctx_send(ctx, head.data, head.len);
    buf_free(&head);
    buf_consume(&in, res.head_len);
    http_response_head_free(&res);
    if (sent < 0) {
        result = RELAY_BROKEN;
        goto out;
    }

    chunk_scanner_t sc;
    chunk_scanner_init(&sc);
    int done = no_body || remaining == 0;
    const char *data = in.data;
    size_t len = in.len;
    while (!done) {
        if (len == 0) {
            int w = wait_fd(upfd, POLLIN, cfg->proxy_timeout_ms);
            ssize_t n = w > 0 ? recv_some(upfd, chunk, sizeof(chunk)) : -1;
            if (n == 0 && until_eof)
                break;
            if (n <= 0) {
                result = RELAY_BROKEN;
                ctx->keep_alive = 0;
                goto out;
            }
            data = chunk;
            len = (size_t)n;
        }
        size_t take = len;
        if (chunked) {
            size_t ignored = 0;
            long used = chunk_scanner_feed(&sc, data, len, NULL, &ignored, &done);
            if (used < 0) {
                result = RELAY_BROKEN;
                ctx->keep_alive = 0;
                goto out;
            }
            take = (size_t)used;
        } else if (remaining > 0) {
            if ((long long)take > remaining)
                take = (size_t)remaining;
            remaining -= (long long)take;
            done = remaining == 0;
        }
        if (ctx_send(ctx, data, take) < 0) {
            result = RELAY_BROKEN;
            goto out;
        }
        capture(o, data, take);
        data += take;
        len -= take;
        if (len && done)
            break; /* extra bytes after the message: ignored, upstream is closed anyway */
    }
    o->capture_complete = o->capture != NULL;
    result = RELAY_OK;
out:
    buf_free(&in);
    return result;
}

static int send_gateway_error(req_ctx_t *ctx, relay_result_t r) {
    if (r == RELAY_TIMEOUT)
        return respond_error(ctx, 504, NULL);
    if (r == RELAY_BAD_UPSTREAM)
        return respond_error(ctx, 502, NULL);
    return -1;
}

static int build_target(buf_t *target, const char *path, const char *query) {
    if (http_encode_path(target, path) < 0)
        return -1;
    if (query[0])
        return buf_printf(target, "?%s", query);
    return 0;
}

int proxy_reverse(req_ctx_t *ctx, const route_t *route) {
    const config_t *cfg = ctx->srv->cfg;
    http_request_t *req = ctx->req;
    upstream_t *up = route->upstream;
    ctx->handler = "proxy";

    buf_t target, request;
    buf_init(&target);
    buf_init(&request);
    int rc = -1;
    if (build_target(&target, router_upstream_path(route, req->path), req->query) < 0)
        goto done;

    uint32_t tried = 0;
    int last_errno = 0;
    backend_t *b;
    while ((b = upstream_pick(up, now_sec(), &tried)) != NULL) {
        snprintf(ctx->upstream, sizeof(ctx->upstream), "%s:%d", b->host, b->port);
        const char *host = http_request_header(req, "Host");
        char host_buf[300];
        if (!host) {
            snprintf(host_buf, sizeof(host_buf), "%s:%d", b->host, b->port);
            host = host_buf;
        }
        buf_clear(&request);
        if (proxy_build_request(&request, req, target.data, host, ctx->conn->peer) < 0)
            goto done;

        int fd = connect_with_timeout(b->host, b->port, cfg->proxy_connect_timeout_ms);
        if (fd < 0) {
            last_errno = errno;
            log_error("upstream %s %s:%d connect failed: %s", up->name, b->host, b->port,
                      strerror(errno));
            backend_mark_down(b, now_sec(), cfg->fail_timeout);
            continue; /* nothing was sent: safe to try the next backend */
        }
        set_io_timeouts(fd, cfg->proxy_timeout_ms);
        atomic_fetch_add(&b->requests, 1);
        if (send_all(fd, request.data, request.len) < 0) {
            last_errno = errno;
            close(fd);
            backend_mark_down(b, now_sec(), cfg->fail_timeout);
            continue;
        }
        relay_opts_t o = {0};
        relay_result_t r = relay_response(ctx, fd, &o);
        close(fd);
        if (r == RELAY_OK) {
            backend_mark_up(b);
        } else if (r == RELAY_BAD_UPSTREAM) {
            backend_mark_down(b, now_sec(), cfg->fail_timeout);
        } else if (r == RELAY_TIMEOUT) {
            atomic_fetch_add(&b->failures, 1);
            log_error("upstream %s %s:%d timed out after %d ms", up->name, b->host, b->port,
                      cfg->proxy_timeout_ms);
        }
        rc = r == RELAY_OK ? 0 : send_gateway_error(ctx, r);
        goto done;
    }
    rc = respond_error(ctx, last_errno == ETIMEDOUT ? 504 : 502, NULL);
done:
    buf_free(&target);
    buf_free(&request);
    return rc;
}

static int serve_from_cache(req_ctx_t *ctx, const char *blob, size_t len, long long age) {
    const char *sep = NULL;
    for (size_t i = 0; i + 1 < len; i++)
        if (blob[i] == '\r' && blob[i + 1] == '\n' && i >= 2 && blob[i - 2] == '\r' &&
            blob[i - 1] == '\n') {
            sep = blob + i;
            break;
        }
    if (!sep)
        return -1;
    size_t head_len = (size_t)(sep - blob);
    buf_t out;
    buf_init(&out);
    buf_append(&out, blob, head_len);
    buf_printf(&out, "Age: %lld\r\nX-Cache: HIT\r\nConnection: %s\r\n\r\n", age,
               ctx->keep_alive ? "keep-alive" : "close");
    buf_append(&out, sep + 2, len - head_len - 2);
    ctx->status = atoi(blob + 9);
    int rc = ctx_send(ctx, out.data, out.len);
    buf_free(&out);
    return rc;
}

static int request_allows_cache(const http_request_t *req) {
    const char *cc = http_request_header(req, "Cache-Control");
    const char *pragma = http_request_header(req, "Pragma");
    return req->method == HTTP_GET && !http_request_header(req, "Authorization") &&
           !http_header_has_token(cc, "no-cache") && !http_header_has_token(cc, "no-store") &&
           !http_header_has_token(pragma, "no-cache");
}

int proxy_forward(req_ctx_t *ctx) {
    server_t *srv = ctx->srv;
    const config_t *cfg = srv->cfg;
    http_request_t *req = ctx->req;
    ctx->handler = "forward";
    snprintf(ctx->upstream, sizeof(ctx->upstream), "%s:%d", req->host, req->port);

    const char *via = http_request_header(req, "Via");
    if (via && strstr(via, PROXY_VIA_TOKEN))
        return respond_error(ctx, 508, NULL); /* we are proxying to ourselves */

    buf_t target, request, key, captured;
    buf_init(&target);
    buf_init(&request);
    buf_init(&key);
    buf_init(&captured);
    int rc = -1;
    if (build_target(&target, req->path, req->query) < 0)
        goto done;

    int use_cache = srv->cache && request_allows_cache(req);
    if (use_cache) {
        buf_printf(&key, "http://%s:%d%s", req->host, req->port, target.data);
        char *blob = NULL;
        size_t blen = 0;
        long long age = 0;
        if (cache_get(srv->cache, key.data, now_sec(), &blob, &blen, &age)) {
            ctx->handler = "forward-cache";
            rc = serve_from_cache(ctx, blob, blen, age);
            free(blob);
            goto done;
        }
    }

    char host_hdr[300];
    if (req->port == 80)
        snprintf(host_hdr, sizeof(host_hdr), "%s", req->host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%d", req->host, req->port);
    if (proxy_build_request(&request, req, target.data, host_hdr, ctx->conn->peer) < 0)
        goto done;

    int fd = connect_with_timeout(req->host, req->port, cfg->proxy_connect_timeout_ms);
    if (fd < 0) {
        rc = respond_error(ctx, errno == ETIMEDOUT ? 504 : 502, NULL);
        goto done;
    }
    set_io_timeouts(fd, cfg->proxy_timeout_ms);
    if (send_all(fd, request.data, request.len) < 0) {
        close(fd);
        rc = respond_error(ctx, 502, NULL);
        goto done;
    }
    relay_opts_t o = {0};
    o.extra_headers = use_cache ? "X-Cache: MISS\r\n" : srv->cache ? "X-Cache: BYPASS\r\n" : NULL;
    if (use_cache) {
        o.capture = &captured;
        o.capture_limit = cfg->cache_max_object;
    }
    relay_result_t r = relay_response(ctx, fd, &o);
    close(fd);
    if (r == RELAY_OK && o.capture_complete) {
        int ttl = o.cache_ttl > 0 ? o.cache_ttl : cfg->cache_default_ttl;
        cache_put(srv->cache, key.data, captured.data, captured.len, now_sec(), ttl);
    }
    rc = r == RELAY_OK ? 0 : send_gateway_error(ctx, r);
done:
    buf_free(&target);
    buf_free(&request);
    buf_free(&key);
    buf_free(&captured);
    return rc;
}

/* Copy bytes both ways until either side closes or the tunnel idles out. */
static void pump(int a, int b, long long *bytes_to_a) {
    char chunk[16384];
    for (;;) {
        struct pollfd p[2] = {{.fd = a, .events = POLLIN}, {.fd = b, .events = POLLIN}};
        int r = poll(p, 2, TUNNEL_IDLE_MS);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || server_stopping())
            return;
        for (int i = 0; i < 2; i++) {
            if (!p[i].revents)
                continue;
            int from = p[i].fd, to = i == 0 ? b : a;
            ssize_t n = recv_some(from, chunk, sizeof(chunk));
            if (n <= 0 || send_all(to, chunk, (size_t)n) < 0)
                return;
            if (to == a)
                *bytes_to_a += n;
        }
    }
}

int proxy_connect_tunnel(req_ctx_t *ctx) {
    const config_t *cfg = ctx->srv->cfg;
    http_request_t *req = ctx->req;
    ctx->handler = "tunnel";
    ctx->keep_alive = 0;
    snprintf(ctx->upstream, sizeof(ctx->upstream), "%s:%d", req->host, req->port);
    if (req->port != 443 && !cfg->connect_any_port)
        return respond_error(ctx, 403, NULL);
    int fd = connect_with_timeout(req->host, req->port, cfg->proxy_connect_timeout_ms);
    if (fd < 0)
        return respond_error(ctx, errno == ETIMEDOUT ? 504 : 502, NULL);
    static const char ok[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
    ctx->status = 200;
    int rc = ctx_send(ctx, ok, sizeof(ok) - 1);
    conn_t *c = ctx->conn;
    if (rc == 0 && c->in.len) { /* bytes the client sent right after CONNECT */
        rc = send_all(fd, c->in.data, c->in.len);
        buf_clear(&c->in);
    }
    if (rc == 0) {
        set_io_timeouts(fd, 0);
        set_io_timeouts(c->fd, 0);
        pump(c->fd, fd, &ctx->bytes);
    }
    close(fd);
    return rc;
}
