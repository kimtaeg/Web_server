#ifndef ROUTER_H
#define ROUTER_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_BACKENDS 16
#define MAX_UPSTREAMS 16
#define MAX_ROUTES 32

typedef struct {
    char host[256];
    int port;
    _Atomic long long down_until; /* epoch seconds; passive health check */
    _Atomic unsigned long long requests;
    _Atomic unsigned long long failures;
} backend_t;

typedef struct {
    char name[64];
    backend_t backends[MAX_BACKENDS];
    size_t count;
    _Atomic unsigned int rr;
} upstream_t;

typedef enum { ROUTE_PREFIX, ROUTE_HOST } route_kind_t;

typedef struct {
    route_kind_t kind;
    char match[256];
    upstream_t *upstream;
    int strip_prefix;
} route_t;

typedef struct {
    upstream_t upstreams[MAX_UPSTREAMS];
    size_t n_upstreams;
    route_t routes[MAX_ROUTES];
    size_t n_routes;
} router_t;

int parse_host_port(const char *s, char *host, size_t host_cap, int *port);

int router_add_upstream(router_t *r, const char *name, char *const *addrs, size_t n,
                        char *err, size_t err_cap);
int router_add_route(router_t *r, route_kind_t kind, const char *match,
                     const char *upstream_name, int strip, char *err, size_t err_cap);

int router_prefix_matches(const char *prefix, const char *path);
/* Host routes win over path routes; among path routes the longest prefix wins. */
const route_t *router_match(const router_t *r, const char *host_header, const char *path);
/* Path to send upstream: with strip_prefix the matched prefix is removed. */
const char *router_upstream_path(const route_t *route, const char *path);

/* Round-robin over healthy backends not yet in *tried (a bitmask). If every backend is
 * marked down and none was tried yet, one is returned anyway as a probe. NULL when done. */
backend_t *upstream_pick(upstream_t *up, long long now, uint32_t *tried);
void backend_mark_down(backend_t *b, long long now, int fail_timeout);
void backend_mark_up(backend_t *b);

#endif
