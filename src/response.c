#include "response.h"

#include <stdio.h>
#include <string.h>

#include "netutil.h"

int response_build_head(buf_t *out, int status, const char *content_type,
                        long long content_length, int keep_alive, int keepalive_timeout,
                        const char *extra_headers) {
    char date[64];
    http_format_date(now_sec(), date, sizeof(date));
    int rc = buf_printf(out, "HTTP/1.1 %d %s\r\nServer: %s\r\nDate: %s\r\n", status,
                        http_reason(status), SERVER_NAME, date);
    if (rc == 0 && content_type) /* nosniff: browsers must not second-guess our type */
        rc = buf_printf(out, "Content-Type: %s\r\nX-Content-Type-Options: nosniff\r\n",
                        content_type);
    if (rc == 0 && content_length >= 0)
        rc = buf_printf(out, "Content-Length: %lld\r\n", content_length);
    if (rc == 0 && keep_alive)
        rc = buf_printf(out, "Connection: keep-alive\r\nKeep-Alive: timeout=%d\r\n",
                        keepalive_timeout);
    else if (rc == 0)
        rc = buf_append_str(out, "Connection: close\r\n");
    if (rc == 0 && extra_headers)
        rc = buf_append_str(out, extra_headers);
    if (rc == 0)
        rc = buf_append_str(out, "\r\n");
    return rc;
}

int ctx_send(req_ctx_t *ctx, const void *data, size_t len) {
    if (send_all(ctx->conn->fd, data, len) < 0) {
        ctx->keep_alive = 0;
        return -1;
    }
    ctx->bytes += (long long)len;
    return 0;
}

int respond_head(req_ctx_t *ctx, int status, const char *content_type,
                 long long content_length, const char *extra_headers) {
    buf_t head;
    buf_init(&head);
    ctx->status = status;
    int rc = response_build_head(&head, status, content_type, content_length, ctx->keep_alive,
                                 ctx->srv->cfg->keepalive_timeout, extra_headers);
    if (rc == 0)
        rc = ctx_send(ctx, head.data, head.len);
    buf_free(&head);
    return rc;
}

int respond(req_ctx_t *ctx, int status, const char *content_type, const char *body,
            size_t len, const char *extra_headers) {
    /* Head and body in one write: avoids a Nagle/delayed-ACK stall on tiny responses. */
    buf_t out;
    buf_init(&out);
    ctx->status = status;
    int head_only = ctx->req && ctx->req->method == HTTP_HEAD;
    int rc = response_build_head(&out, status, content_type, (long long)len, ctx->keep_alive,
                                 ctx->srv->cfg->keepalive_timeout, extra_headers);
    if (rc == 0 && !head_only && len)
        rc = buf_append(&out, body, len);
    if (rc == 0)
        rc = ctx_send(ctx, out.data, out.len);
    buf_free(&out);
    return rc;
}

int respond_error(req_ctx_t *ctx, int status, const char *extra_headers) {
    char body[512];
    const char *reason = http_reason(status);
    int n = snprintf(body, sizeof(body),
                     "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>%d %s</title>"
                     "</head>\n<body><h1>%d %s</h1><hr><address>%s</address></body></html>\n",
                     status, reason, status, reason, SERVER_NAME);
    if (status >= 400 && status != 404 && status != 403 && status != 405)
        ctx->keep_alive = 0; /* after malformed input or failures, start clean */
    return respond(ctx, status, "text/html; charset=utf-8", body, (size_t)n, extra_headers);
}
