#include "handler.h"

#include <string.h>

#include "cgi.h"
#include "netutil.h"
#include "proxy.h"
#include "response.h"
#include "static_file.h"

dispatch_t dispatch_decide(const config_t *cfg, const http_request_t *req,
                           const route_t **route) {
    *route = NULL;
    if (req->form == TARGET_AUTHORITY)
        return cfg->forward_proxy ? DISPATCH_TUNNEL : DISPATCH_PROXY_DISABLED;
    if (req->form == TARGET_ABSOLUTE)
        return cfg->forward_proxy ? DISPATCH_FORWARD : DISPATCH_PROXY_DISABLED;
    if (req->form == TARGET_ASTERISK)
        return DISPATCH_OPTIONS;
    if (cfg->status_page && strcmp(req->path, STATUS_PATH) == 0)
        return DISPATCH_STATUS;
    *route = router_match(&cfg->router, http_request_header(req, "Host"), req->path);
    if (*route)
        return DISPATCH_REVERSE;
    if (cfg->cgi_enabled && router_prefix_matches(cfg->cgi_prefix, req->path))
        return DISPATCH_CGI;
    return DISPATCH_STATIC;
}

static int status_page(req_ctx_t *ctx) {
    server_t *s = ctx->srv;
    const config_t *cfg = s->cfg;
    server_stats_t *st = &s->stats;
    ctx->handler = "status";
    if (ctx->req->method != HTTP_GET && ctx->req->method != HTTP_HEAD)
        return respond_error(ctx, 405, "Allow: GET, HEAD\r\n");
    buf_t b;
    buf_init(&b);
    buf_printf(&b,
               "{\n  \"server\": \"%s\",\n  \"uptime_sec\": %lld,\n  \"threads\": %d,\n"
               "  \"queued_jobs\": %zu,\n  \"active_connections\": %lld,\n"
               "  \"total_connections\": %llu,\n  \"rejected_connections\": %llu,\n"
               "  \"total_requests\": %llu,\n  \"bytes_out\": %llu,\n"
               "  \"responses\": {\"1xx\": %llu, \"2xx\": %llu, \"3xx\": %llu, \"4xx\": %llu, "
               "\"5xx\": %llu}",
               SERVER_NAME, now_sec() - s->started_at, threadpool_size(s->pool),
               threadpool_pending(s->pool), atomic_load(&st->active_connections),
               atomic_load(&st->connections), atomic_load(&st->rejected),
               atomic_load(&st->requests), atomic_load(&st->bytes_out),
               atomic_load(&st->by_class[1]), atomic_load(&st->by_class[2]),
               atomic_load(&st->by_class[3]), atomic_load(&st->by_class[4]),
               atomic_load(&st->by_class[5]));
    if (s->cache) {
        cache_stats_t cs;
        cache_stats(s->cache, &cs);
        buf_printf(&b,
                   ",\n  \"cache\": {\"entries\": %zu, \"bytes\": %zu, \"hits\": %llu, "
                   "\"misses\": %llu, \"evictions\": %llu}",
                   cs.entries, cs.bytes, cs.hits, cs.misses, cs.evictions);
    }
    buf_append_str(&b, ",\n  \"upstreams\": [");
    long long now = now_sec();
    for (size_t i = 0; i < cfg->router.n_upstreams; i++) {
        const upstream_t *up = &cfg->router.upstreams[i];
        buf_printf(&b, "%s\n    {\"name\": \"%s\", \"backends\": [", i ? "," : "", up->name);
        for (size_t j = 0; j < up->count; j++) {
            const backend_t *be = &up->backends[j];
            buf_printf(&b,
                       "%s{\"addr\": \"%s:%d\", \"healthy\": %s, \"requests\": %llu, "
                       "\"failures\": %llu}",
                       j ? ", " : "", be->host, be->port,
                       atomic_load(&be->down_until) <= now ? "true" : "false",
                       atomic_load(&be->requests), atomic_load(&be->failures));
        }
        buf_append_str(&b, "]}");
    }
    buf_append_str(&b, cfg->router.n_upstreams ? "\n  ]\n}\n" : "]\n}\n");
    int rc = respond(ctx, 200, "application/json", b.data, b.len, "Cache-Control: no-store\r\n");
    buf_free(&b);
    return rc;
}

int handle_request(req_ctx_t *ctx) {
    const route_t *route = NULL;
    http_request_t *req = ctx->req;
    if (req->method == HTTP_TRACE) {
        ctx->handler = "reject";
        return respond_error(ctx, 405, "Allow: GET, HEAD, POST, OPTIONS\r\n");
    }
    switch (dispatch_decide(ctx->srv->cfg, req, &route)) {
    case DISPATCH_TUNNEL: return proxy_connect_tunnel(ctx);
    case DISPATCH_FORWARD: return proxy_forward(ctx);
    case DISPATCH_PROXY_DISABLED:
        ctx->handler = "reject";
        return respond_error(ctx, 403, NULL);
    case DISPATCH_OPTIONS:
        ctx->handler = "options";
        return respond(ctx, 200, NULL, NULL, 0, "Allow: GET, HEAD, POST, OPTIONS\r\n");
    case DISPATCH_STATUS: return status_page(ctx);
    case DISPATCH_REVERSE: return proxy_reverse(ctx, route);
    case DISPATCH_CGI: return cgi_handle(ctx);
    case DISPATCH_STATIC: break;
    }
    return static_serve(ctx);
}
