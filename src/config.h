#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <stddef.h>

#include "router.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char bind_addr[64];
    int port;
    char root[PATH_MAX];
    char root_real[PATH_MAX]; /* realpath(root), used for containment checks */
    char index_file[64];

    int threads;
    int max_connections;
    int keepalive_timeout;  /* seconds an idle keep-alive connection may wait */
    int request_timeout;    /* seconds to receive one complete request */
    int max_keepalive_requests;
    size_t max_body;

    int cgi_enabled;
    char cgi_prefix[128];
    char cgi_dir[PATH_MAX];
    int cgi_timeout_ms;

    int proxy_connect_timeout_ms;
    int proxy_timeout_ms;
    int fail_timeout; /* seconds a failed backend is skipped */
    router_t router;

    int forward_proxy;
    int connect_any_port; /* CONNECT is limited to port 443 unless set */
    int cache_enabled;
    size_t cache_max_bytes;
    size_t cache_max_object;
    int cache_default_ttl;

    int status_page;
    int access_log;
} config_t;

void config_defaults(config_t *cfg);
/* Apply one "directive arg..." line (modifies line). Returns 0 or -1 with err set. */
int config_apply_line(config_t *cfg, char *line, char *err, size_t err_cap);
int config_load_file(config_t *cfg, const char *path, char *err, size_t err_cap);
/* Returns 0 to run, 1 when --help was printed, -1 on error. */
int config_from_args(config_t *cfg, int argc, char **argv, char *err, size_t err_cap);
/* Resolves paths and checks consistency after all sources were applied. */
int config_finalize(config_t *cfg, char *err, size_t err_cap);
int parse_size(const char *s, size_t *out);
void config_usage(const char *prog);

#endif
