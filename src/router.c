#include "router.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int parse_host_port(const char *s, char *host, size_t host_cap, int *port) {
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s)
        return -1;
    size_t hlen = (size_t)(colon - s);
    if (hlen >= host_cap)
        return -1;
    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (!colon[1] || *end || p < 1 || p > 65535)
        return -1;
    memcpy(host, s, hlen);
    host[hlen] = '\0';
    *port = (int)p;
    return 0;
}

static upstream_t *find_upstream(router_t *r, const char *name) {
    for (size_t i = 0; i < r->n_upstreams; i++)
        if (strcmp(r->upstreams[i].name, name) == 0)
            return &r->upstreams[i];
    return NULL;
}

int router_add_upstream(router_t *r, const char *name, char *const *addrs, size_t n,
                        char *err, size_t err_cap) {
    if (find_upstream(r, name)) {
        snprintf(err, err_cap, "duplicate upstream '%s'", name);
        return -1;
    }
    if (r->n_upstreams >= MAX_UPSTREAMS || n == 0 || n > MAX_BACKENDS ||
        strlen(name) >= sizeof(r->upstreams[0].name)) {
        snprintf(err, err_cap, "upstream '%s': need 1..%d backends", name, MAX_BACKENDS);
        return -1;
    }
    upstream_t *up = &r->upstreams[r->n_upstreams];
    memset(up, 0, sizeof(*up));
    snprintf(up->name, sizeof(up->name), "%s", name);
    for (size_t i = 0; i < n; i++) {
        backend_t *b = &up->backends[i];
        if (parse_host_port(addrs[i], b->host, sizeof(b->host), &b->port) < 0) {
            snprintf(err, err_cap, "upstream '%s': bad address '%s' (want host:port)", name,
                     addrs[i]);
            return -1;
        }
        atomic_init(&b->down_until, 0);
        atomic_init(&b->requests, 0);
        atomic_init(&b->failures, 0);
    }
    up->count = n;
    atomic_init(&up->rr, 0);
    r->n_upstreams++;
    return 0;
}

int router_add_route(router_t *r, route_kind_t kind, const char *match,
                     const char *upstream_name, int strip, char *err, size_t err_cap) {
    upstream_t *up = find_upstream(r, upstream_name);
    if (!up) {
        snprintf(err, err_cap, "route '%s': unknown upstream '%s'", match, upstream_name);
        return -1;
    }
    if (r->n_routes >= MAX_ROUTES || strlen(match) >= sizeof(r->routes[0].match)) {
        snprintf(err, err_cap, "route '%s': too many routes or too long", match);
        return -1;
    }
    if (kind == ROUTE_PREFIX && match[0] != '/') {
        snprintf(err, err_cap, "route '%s': prefix must start with '/'", match);
        return -1;
    }
    route_t *rt = &r->routes[r->n_routes++];
    rt->kind = kind;
    rt->upstream = up;
    rt->strip_prefix = strip;
    snprintf(rt->match, sizeof(rt->match), "%s", match);
    if (kind == ROUTE_PREFIX) {
        size_t len = strlen(rt->match);
        while (len > 1 && rt->match[len - 1] == '/')
            rt->match[--len] = '\0';
    }
    return 0;
}

/* "/api" matches "/api" and "/api/..." but not "/apix". */
int router_prefix_matches(const char *prefix, const char *path) {
    size_t n = strlen(prefix);
    if (n == 1 && prefix[0] == '/')
        return 1;
    return strncmp(prefix, path, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static int host_equals(const char *route_host, const char *host_header) {
    size_t n = strcspn(host_header, ":");
    if (host_header[0] == '[') { /* IPv6 literal: compare up to the bracket */
        const char *rb = strchr(host_header, ']');
        n = rb ? (size_t)(rb - host_header + 1) : strlen(host_header);
    }
    return strlen(route_host) == n && strncasecmp(route_host, host_header, n) == 0;
}

const route_t *router_match(const router_t *r, const char *host_header, const char *path) {
    const route_t *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < r->n_routes; i++) {
        const route_t *rt = &r->routes[i];
        if (rt->kind == ROUTE_HOST) {
            if (host_header && host_equals(rt->match, host_header))
                return rt;
        } else if (router_prefix_matches(rt->match, path)) {
            size_t len = strlen(rt->match);
            if (!best || len > best_len) {
                best = rt;
                best_len = len;
            }
        }
    }
    return best;
}

const char *router_upstream_path(const route_t *route, const char *path) {
    if (route->kind != ROUTE_PREFIX || !route->strip_prefix || strcmp(route->match, "/") == 0)
        return path;
    const char *rest = path + strlen(route->match);
    return *rest ? rest : "/";
}

backend_t *upstream_pick(upstream_t *up, long long now, uint32_t *tried) {
    if (up->count == 0)
        return NULL;
    unsigned start = atomic_fetch_add(&up->rr, 1);
    for (size_t i = 0; i < up->count; i++) {
        size_t idx = (start + i) % up->count;
        if (*tried & (1u << idx))
            continue;
        if (atomic_load(&up->backends[idx].down_until) <= now) {
            *tried |= 1u << idx;
            return &up->backends[idx];
        }
    }
    if (*tried == 0) {
        size_t idx = start % up->count;
        *tried |= 1u << idx;
        return &up->backends[idx];
    }
    return NULL;
}

void backend_mark_down(backend_t *b, long long now, int fail_timeout) {
    atomic_fetch_add(&b->failures, 1);
    atomic_store(&b->down_until, now + fail_timeout);
}

void backend_mark_up(backend_t *b) {
    if (atomic_load(&b->down_until) != 0)
        atomic_store(&b->down_until, 0);
}
