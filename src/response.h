#ifndef RESPONSE_H
#define RESPONSE_H

#include "buffer.h"
#include "server.h"

#define SERVER_NAME "cwebserv/1.0"

/* Status line + standard headers + extra (each line CRLF-terminated) + blank line. */
int response_build_head(buf_t *out, int status, const char *content_type,
                        long long content_length, int keep_alive, int keepalive_timeout,
                        const char *extra_headers);

int respond_head(req_ctx_t *ctx, int status, const char *content_type,
                 long long content_length, const char *extra_headers);
int respond(req_ctx_t *ctx, int status, const char *content_type, const char *body,
            size_t len, const char *extra_headers);
/* Small HTML error page. Text is fixed per status, so nothing from the request is echoed. */
int respond_error(req_ctx_t *ctx, int status, const char *extra_headers);
int ctx_send(req_ctx_t *ctx, const void *data, size_t len);

#endif
