/*
 * router.c — (Host, 경로 접두사) 기반 핸들러 선택. 규칙은 router.h 참고.
 */
#include "router.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

void router_init(router_t *r, conn_handler_fn fallback, void *fallback_ctx)
{
    memset(r, 0, sizeof *r);
    r->fallback = fallback;
    r->fallback_ctx = fallback_ctx;
}

int router_add_host(router_t *r, const char *host, const char *prefix, conn_handler_fn fn,
                    void *ctx)
{
    size_t len = strlen(prefix);
    size_t hl = host ? strlen(host) : 0;
    route_t *rt;
    size_t i;

    if (r->n >= ROUTER_MAX_ROUTES || len == 0 || prefix[0] != '/' || len >= ROUTER_MAX_PREFIX ||
        hl >= ROUTER_MAX_PREFIX || fn == NULL)
        return -1;
    while (len > 0 && prefix[len - 1] == '/') /* "/cgi-bin/" → "/cgi-bin" */
        len--;
    rt = &r->routes[r->n++];
    memcpy(rt->prefix, prefix, len);
    rt->prefix[len] = '\0';
    rt->len = len;
    hl = host ? router_host_name_len(host, hl) : 0; /* 규칙에 포트를 적어도 무시 */
    for (i = 0; i < hl; i++)
        rt->host[i] = (char)tolower((unsigned char)host[i]);
    rt->host[hl] = '\0';
    rt->hlen = hl;
    rt->fn = fn;
    rt->ctx = ctx;
    return 0;
}

int router_add(router_t *r, const char *prefix, conn_handler_fn fn, void *ctx)
{
    return router_add_host(r, NULL, prefix, fn, ctx);
}

size_t router_host_name_len(const char *host, size_t n)
{
    size_t i;

    if (n > 0 && host[0] == '[') { /* [IPv6]:port */
        for (i = 0; i < n; i++)
            if (host[i] == ']')
                return i + 1;
        return n;
    }
    for (i = 0; i < n; i++)
        if (host[i] == ':')
            return i;
    return n;
}

const route_t *router_match(const router_t *r, const char *host, size_t host_len, const char *path,
                            size_t path_len)
{
    const route_t *best = NULL;
    size_t hn = host ? router_host_name_len(host, host_len) : 0;
    size_t i;

    for (i = 0; i < r->n; i++) {
        const route_t *rt = &r->routes[i];

        if (rt->hlen > 0 && (rt->hlen != hn || strncasecmp(rt->host, host, hn) != 0))
            continue; /* Host 조건 불일치 */
        if (rt->len > path_len || memcmp(path, rt->prefix, rt->len) != 0)
            continue;
        /* 경계: 접두사 바로 뒤가 끝이거나 '/' 여야 한다 */
        if (rt->len != path_len && path[rt->len] != '/')
            continue;
        if (best == NULL || rt->len > best->len || (rt->len == best->len && rt->hlen > 0 && best->hlen == 0))
            best = rt;
    }
    return best;
}

int router_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res)
{
    const router_t *r = ctx;
    size_t hl = 0;
    const char *host = http_header_get(req, "Host", &hl);
    const route_t *rt;

    if (host == NULL && req->is_absolute) { /* absolute-form 이면 authority 가 Host 역할 */
        host = req->authority;
        hl = req->authority_len;
    }
    rt = router_match(r, host, hl, req->path, req->path_len);
    if (rt != NULL)
        return rt->fn(rt->ctx, fd, req, ci, keep_alive, res);
    return r->fallback(r->fallback_ctx, fd, req, ci, keep_alive, res);
}
