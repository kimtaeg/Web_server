/*
 * router.c — 경로 접두사 기반 핸들러 선택. 규칙은 router.h 참고.
 */
#include "router.h"

#include <string.h>

void router_init(router_t *r, conn_handler_fn fallback, void *fallback_ctx)
{
    memset(r, 0, sizeof *r);
    r->fallback = fallback;
    r->fallback_ctx = fallback_ctx;
}

int router_add(router_t *r, const char *prefix, conn_handler_fn fn, void *ctx)
{
    size_t len = strlen(prefix);
    route_t *rt;

    if (r->n >= ROUTER_MAX_ROUTES || len == 0 || prefix[0] != '/' || len >= ROUTER_MAX_PREFIX ||
        fn == NULL)
        return -1;
    while (len > 0 && prefix[len - 1] == '/') /* "/cgi-bin/" → "/cgi-bin" */
        len--;
    rt = &r->routes[r->n++];
    memcpy(rt->prefix, prefix, len);
    rt->prefix[len] = '\0';
    rt->len = len;
    rt->fn = fn;
    rt->ctx = ctx;
    return 0;
}

const route_t *router_match(const router_t *r, const char *path, size_t path_len)
{
    const route_t *best = NULL;
    size_t i;

    for (i = 0; i < r->n; i++) {
        const route_t *rt = &r->routes[i];

        if (rt->len > path_len || memcmp(path, rt->prefix, rt->len) != 0)
            continue;
        /* 경계: 접두사 바로 뒤가 끝이거나 '/' 여야 한다 */
        if (rt->len != path_len && path[rt->len] != '/')
            continue;
        if (best == NULL || rt->len > best->len)
            best = rt;
    }
    return best;
}

int router_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res)
{
    const router_t *r = ctx;
    const route_t *rt = router_match(r, req->path, req->path_len);

    if (rt != NULL)
        return rt->fn(rt->ctx, fd, req, ci, keep_alive, res);
    return r->fallback(r->fallback_ctx, fd, req, ci, keep_alive, res);
}
