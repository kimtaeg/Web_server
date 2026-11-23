#ifndef HANDLER_H
#define HANDLER_H

#include "server.h"

#define STATUS_PATH "/__status"

/* Kind of handler a request goes to; the order of checks is the routing policy. */
typedef enum {
    DISPATCH_TUNNEL,
    DISPATCH_FORWARD,
    DISPATCH_PROXY_DISABLED,
    DISPATCH_OPTIONS,
    DISPATCH_STATUS,
    DISPATCH_REVERSE,
    DISPATCH_CGI,
    DISPATCH_STATIC
} dispatch_t;

dispatch_t dispatch_decide(const config_t *cfg, const http_request_t *req,
                           const route_t **route);
/* Runs the handler; afterwards ctx->keep_alive says whether the connection survives. */
int handle_request(req_ctx_t *ctx);

#endif
