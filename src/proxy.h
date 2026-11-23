#ifndef PROXY_H
#define PROXY_H

#include "buffer.h"
#include "http.h"
#include "router.h"
#include "server.h"

#define PROXY_VIA_TOKEN "cwebserv"

/* Headers that describe one hop and must not be forwarded (RFC 7230 6.1), plus any
 * header named in the message's own Connection header. */
int proxy_is_hop_header(const char *name, const char *connection_value);

/* Upstream request: origin-form target, Host, X-Forwarded-*, Via, Connection: close. */
int proxy_build_request(buf_t *out, const http_request_t *req, const char *target,
                        const char *host_header, const char *client_ip);

/* Status line and end-to-end headers of an upstream response, without Connection or
 * the terminating blank line. */
int proxy_response_head_lines(buf_t *out, const http_response_head_t *res);

int proxy_reverse(req_ctx_t *ctx, const route_t *route);
int proxy_forward(req_ctx_t *ctx);
int proxy_connect_tunnel(req_ctx_t *ctx);

#endif
