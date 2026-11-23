#include <stdatomic.h>
#include <stdlib.h>

#include "cache.h"
#include "mime.h"
#include "router.h"
#include "test.h"

void suite_mime(void) {
    CHECK_STR(mime_type_for_path("/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("/INDEX.HTM"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("/s/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for_path("app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path("/img/a.PNG"), "image/png");
    CHECK_STR(mime_type_for_path("/img/a.jpg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("/img/a.jpeg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("/img/a.svg"), "image/svg+xml");
    CHECK_STR(mime_type_for_path("/data.json"), "application/json");
    CHECK_STR(mime_type_for_path("/archive.tar.gz"), "application/gzip");
    CHECK_STR(mime_type_for_path("/Makefile"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/.bashrc"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/dir.d/file"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/trailing."), "application/octet-stream");
}

static router_t r;

static void setup_router(void) {
    char err[256];
    memset(&r, 0, sizeof(r));
    char *api[] = {"127.0.0.1:9001", "127.0.0.1:9002", "127.0.0.1:9003"};
    char *st[] = {"localhost:9100"};
    CHECK_INT(router_add_upstream(&r, "api", api, 3, err, sizeof(err)), 0);
    CHECK_INT(router_add_upstream(&r, "static", st, 1, err, sizeof(err)), 0);
    CHECK_INT(router_add_route(&r, ROUTE_PREFIX, "/api/", "api", 1, err, sizeof(err)), 0);
    CHECK_INT(router_add_route(&r, ROUTE_PREFIX, "/api/v2", "static", 0, err, sizeof(err)), 0);
    CHECK_INT(router_add_route(&r, ROUTE_PREFIX, "/static", "static", 0, err, sizeof(err)), 0);
    CHECK_INT(router_add_route(&r, ROUTE_HOST, "admin.test", "static", 0, err, sizeof(err)), 0);
}

static void test_router_config_errors(void) {
    char err[256];
    router_t bad;
    memset(&bad, 0, sizeof(bad));
    char *addr_noport[] = {"127.0.0.1"};
    char *addr_badport[] = {"127.0.0.1:70000"};
    char *ok[] = {"h:1"};
    CHECK_INT(router_add_upstream(&bad, "x", addr_noport, 1, err, sizeof(err)), -1);
    CHECK_INT(router_add_upstream(&bad, "x", addr_badport, 1, err, sizeof(err)), -1);
    CHECK_INT(router_add_upstream(&bad, "x", ok, 1, err, sizeof(err)), 0);
    CHECK_INT(router_add_upstream(&bad, "x", ok, 1, err, sizeof(err)), -1); /* duplicate */
    CHECK_INT(router_add_route(&bad, ROUTE_PREFIX, "/a", "nope", 0, err, sizeof(err)), -1);
    CHECK_INT(router_add_route(&bad, ROUTE_PREFIX, "a", "x", 0, err, sizeof(err)), -1);
    char host[16];
    int port;
    CHECK_INT(parse_host_port("a.b:80", host, sizeof(host), &port), 0);
    CHECK_STR(host, "a.b");
    CHECK_INT(port, 80);
    CHECK_INT(parse_host_port(":80", host, sizeof(host), &port), -1);
    CHECK_INT(parse_host_port("h:", host, sizeof(host), &port), -1);
    CHECK_INT(parse_host_port("h:8x", host, sizeof(host), &port), -1);
}

static void test_router_match(void) {
    const route_t *rt;
    rt = router_match(&r, "example.com", "/api/users");
    CHECK(rt && strcmp(rt->upstream->name, "api") == 0);
    CHECK_STR(router_upstream_path(rt, "/api/users"), "/users");
    CHECK_STR(router_upstream_path(rt, "/api"), "/");
    rt = router_match(&r, "example.com", "/api");
    CHECK(rt && strcmp(rt->match, "/api") == 0);
    rt = router_match(&r, NULL, "/api/v2/items"); /* longest prefix wins */
    CHECK(rt && strcmp(rt->match, "/api/v2") == 0);
    CHECK_STR(router_upstream_path(rt, "/api/v2/items"), "/api/v2/items"); /* no strip */
    CHECK(router_match(&r, "example.com", "/apix") == NULL); /* segment boundary */
    CHECK(router_match(&r, "example.com", "/") == NULL);
    rt = router_match(&r, "ADMIN.test:8080", "/api/users"); /* host beats path */
    CHECK(rt && rt->kind == ROUTE_HOST);
    CHECK(router_match(&r, "admin.test.evil", "/x") == NULL);
    CHECK(router_prefix_matches("/", "/anything"));
    CHECK(!router_prefix_matches("/a/b", "/a"));
}

static void test_round_robin(void) {
    upstream_t *up = &r.upstreams[0];
    int counts[3] = {0};
    for (int i = 0; i < 30; i++) {
        uint32_t tried = 0;
        backend_t *b = upstream_pick(up, 1000, &tried);
        counts[b - up->backends]++;
    }
    CHECK_INT(counts[0], 10);
    CHECK_INT(counts[1], 10);
    CHECK_INT(counts[2], 10);

    backend_mark_down(&up->backends[1], 1000, 10);
    int picked1 = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t tried = 0;
        if (upstream_pick(up, 1005, &tried) == &up->backends[1])
            picked1++;
    }
    CHECK_INT(picked1, 0);                                  /* skipped while down */
    CHECK_INT(atomic_load(&up->backends[1].failures), 1);
    uint32_t tried = 0;
    int seen = 0;
    while (upstream_pick(up, 1005, &tried))
        seen++;
    CHECK_INT(seen, 2);                                     /* retry walks healthy ones once */

    int back = 0;
    for (int i = 0; i < 3; i++) {
        tried = 0;
        if (upstream_pick(up, 1011, &tried) == &up->backends[1])
            back++;
    }
    CHECK_INT(back, 1);                                     /* fail_timeout elapsed */

    for (int i = 0; i < 3; i++)
        backend_mark_down(&up->backends[i], 2000, 10);
    tried = 0;
    CHECK(upstream_pick(up, 2001, &tried) != NULL);         /* all down: probe one */
    CHECK(upstream_pick(up, 2001, &tried) == NULL);
    for (int i = 0; i < 3; i++)
        backend_mark_up(&up->backends[i]);
}

void suite_router(void) {
    setup_router();
    test_router_config_errors();
    test_router_match();
    test_round_robin();
}

void suite_cache(void) {
    cache_t *c = cache_create(100, 60);
    char *data;
    size_t len;
    long long age;
    CHECK_INT(cache_get(c, "k", 0, &data, &len, &age), 0);
    CHECK_INT(cache_put(c, "a", "AAAAAAAAAA", 10, 100, 30), 0);
    CHECK_INT(cache_get(c, "a", 110, &data, &len, &age), 1);
    CHECK_INT(len, 10);
    CHECK_INT(age, 10);
    CHECK(memcmp(data, "AAAAAAAAAA", 10) == 0);
    free(data);
    CHECK_INT(cache_get(c, "a", 130, &data, &len, &age), 0); /* expired at 100+30 */

    CHECK_INT(cache_put(c, "big", "x", 61, 0, 30), -1);       /* over max_object */
    CHECK_INT(cache_put(c, "zero", "x", 1, 0, 0), -1);        /* ttl 0 */

    char blob[40];
    memset(blob, 'b', sizeof(blob));
    CHECK_INT(cache_put(c, "1", blob, 40, 0, 100), 0);
    CHECK_INT(cache_put(c, "2", blob, 40, 0, 100), 0);
    CHECK_INT(cache_get(c, "1", 1, &data, &len, &age), 1); /* 1 becomes most recent */
    free(data);
    CHECK_INT(cache_put(c, "3", blob, 40, 0, 100), 0);     /* 120 > 100: evicts LRU "2" */
    CHECK_INT(cache_get(c, "2", 1, &data, &len, &age), 0);
    CHECK_INT(cache_get(c, "1", 1, &data, &len, &age), 1);
    free(data);
    CHECK_INT(cache_put(c, "1", "new", 3, 5, 100), 0);     /* replace */
    CHECK_INT(cache_get(c, "1", 6, &data, &len, &age), 1);
    CHECK_INT(len, 3);
    free(data);
    cache_stats_t st;
    cache_stats(c, &st);
    CHECK_INT(st.entries, 2);
    CHECK_INT(st.bytes, 43);
    CHECK_INT(st.evictions, 1);
    CHECK(st.hits >= 4 && st.misses >= 3);
    cache_destroy(c);

    CHECK_INT(cache_ttl_from_cache_control(NULL), -1);
    CHECK_INT(cache_ttl_from_cache_control("public, max-age=120"), 120);
    CHECK_INT(cache_ttl_from_cache_control("max-age=\"30\""), 30);
    CHECK_INT(cache_ttl_from_cache_control("s-maxage=5, max-age = 7"), 7);
    CHECK_INT(cache_ttl_from_cache_control("no-store"), 0);
    CHECK_INT(cache_ttl_from_cache_control("private, max-age=60"), 0);
    CHECK_INT(cache_ttl_from_cache_control("no-cache"), 0);
    CHECK_INT(cache_ttl_from_cache_control("public"), -1);
    CHECK_INT(cache_ttl_from_cache_control("max-age=abc"), 0);
}
