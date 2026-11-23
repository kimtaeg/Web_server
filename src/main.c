#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "log.h"
#include "server.h"

static config_t cfg; /* ~100 KB with the router tables: keep it off the stack */

int main(int argc, char **argv) {
    char err[512];
    config_defaults(&cfg);
    int rc = config_from_args(&cfg, argc, argv, err, sizeof(err));
    if (rc == 1)
        return 0;
    if (rc < 0 || config_finalize(&cfg, err, sizeof(err)) < 0) {
        fprintf(stderr, "config error: %s\n", err);
        config_usage(argv[0]);
        return 2;
    }
    log_set_access(cfg.access_log);

    server_t srv;
    if (server_init(&srv, &cfg, err, sizeof(err)) < 0) {
        fprintf(stderr, "startup failed: %s\n", err);
        server_destroy(&srv);
        return 1;
    }
    server_install_signals(&srv);
    for (size_t i = 0; i < cfg.router.n_routes; i++) {
        const route_t *r = &cfg.router.routes[i];
        log_info("route %s %s -> upstream %s (%zu backends)%s",
                 r->kind == ROUTE_HOST ? "host" : "prefix", r->match, r->upstream->name,
                 r->upstream->count, r->strip_prefix ? " strip" : "");
    }
    if (cfg.cgi_enabled)
        log_info("cgi %s -> %s", cfg.cgi_prefix, cfg.cgi_dir);
    if (cfg.forward_proxy)
        log_info("forward proxy enabled%s", cfg.cache_enabled ? " with cache" : "");
    rc = server_run(&srv);
    server_destroy(&srv);
    return rc == 0 ? 0 : 1;
}
