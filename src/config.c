#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_ARGS 24

void config_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->bind_addr, sizeof(cfg->bind_addr), "0.0.0.0");
    cfg->port = 8080;
    snprintf(cfg->root, sizeof(cfg->root), "./www");
    snprintf(cfg->index_file, sizeof(cfg->index_file), "index.html");
    cfg->threads = 8;
    cfg->max_connections = 4096;
    cfg->keepalive_timeout = 5;
    cfg->request_timeout = 10;
    cfg->max_keepalive_requests = 1000;
    cfg->max_body = 10u * 1024 * 1024;
    cfg->cgi_timeout_ms = 5000;
    cfg->proxy_connect_timeout_ms = 1000;
    cfg->proxy_timeout_ms = 5000;
    cfg->fail_timeout = 10;
    cfg->cache_max_bytes = 64u * 1024 * 1024;
    cfg->cache_max_object = 1024 * 1024;
    cfg->cache_default_ttl = 60;
    cfg->status_page = 1;
    cfg->access_log = 1;
}

int parse_size(const char *s, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || errno || s[0] == '-')
        return -1;
    unsigned long long mul = 1;
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': mul = 1024ULL; break;
        case 'm': mul = 1024ULL * 1024; break;
        case 'g': mul = 1024ULL * 1024 * 1024; break;
        default: return -1;
        }
        if (end[1] && !(tolower((unsigned char)end[1]) == 'b' && !end[2]))
            return -1;
    }
    if (v > (unsigned long long)((size_t)-1) / mul)
        return -1;
    *out = (size_t)(v * mul);
    return 0;
}

static int parse_int(const char *s, int lo, int hi, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_bool(const char *s, int *out) {
    if (!strcasecmp(s, "on") || !strcasecmp(s, "yes") || !strcmp(s, "1") ||
        !strcasecmp(s, "true"))
        *out = 1;
    else if (!strcasecmp(s, "off") || !strcasecmp(s, "no") || !strcmp(s, "0") ||
             !strcasecmp(s, "false"))
        *out = 0;
    else
        return -1;
    return 0;
}

static int copy_str(char *dst, size_t cap, const char *src) {
    if (strlen(src) >= cap)
        return -1;
    memcpy(dst, src, strlen(src) + 1);
    return 0;
}

static int tokenize(char *line, char **argv) {
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (argc == MAX_ARGS)
            return -1;
        argv[argc++] = tok;
    }
    return argc;
}

static int apply_listen(config_t *cfg, const char *v) {
    const char *colon = strrchr(v, ':');
    if (colon) {
        size_t n = (size_t)(colon - v);
        if (n == 0 || n >= sizeof(cfg->bind_addr))
            return -1;
        memcpy(cfg->bind_addr, v, n);
        cfg->bind_addr[n] = '\0';
        v = colon + 1;
    }
    return parse_int(v, 1, 65535, &cfg->port);
}

int config_apply_line(config_t *cfg, char *line, char *err, size_t err_cap) {
    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv);
    if (argc < 0) {
        snprintf(err, err_cap, "too many arguments");
        return -1;
    }
    if (argc == 0)
        return 0;
    const char *d = argv[0];
    int ok = -1;
    int one = (argc == 2);

#define IS(name) (strcmp(d, name) == 0)
    if (IS("listen") && one) ok = apply_listen(cfg, argv[1]);
    else if (IS("bind") && one) ok = copy_str(cfg->bind_addr, sizeof(cfg->bind_addr), argv[1]);
    else if (IS("root") && one) ok = copy_str(cfg->root, sizeof(cfg->root), argv[1]);
    else if (IS("index") && one) ok = copy_str(cfg->index_file, sizeof(cfg->index_file), argv[1]);
    else if (IS("threads") && one) ok = parse_int(argv[1], 1, 1024, &cfg->threads);
    else if (IS("max_connections") && one) ok = parse_int(argv[1], 1, 1000000, &cfg->max_connections);
    else if (IS("keepalive_timeout") && one) ok = parse_int(argv[1], 0, 3600, &cfg->keepalive_timeout);
    else if (IS("request_timeout") && one) ok = parse_int(argv[1], 1, 3600, &cfg->request_timeout);
    else if (IS("max_keepalive_requests") && one) ok = parse_int(argv[1], 1, 1000000, &cfg->max_keepalive_requests);
    else if (IS("max_body_size") && one) ok = parse_size(argv[1], &cfg->max_body);
    else if (IS("cgi_timeout") && one) ok = parse_int(argv[1], 1, 600000, &cfg->cgi_timeout_ms);
    else if (IS("proxy_connect_timeout") && one) ok = parse_int(argv[1], 1, 600000, &cfg->proxy_connect_timeout_ms);
    else if (IS("proxy_timeout") && one) ok = parse_int(argv[1], 1, 600000, &cfg->proxy_timeout_ms);
    else if (IS("fail_timeout") && one) ok = parse_int(argv[1], 0, 3600, &cfg->fail_timeout);
    else if (IS("forward_proxy") && one) ok = parse_bool(argv[1], &cfg->forward_proxy);
    else if (IS("connect_any_port") && one) ok = parse_bool(argv[1], &cfg->connect_any_port);
    else if (IS("cache") && one) ok = parse_bool(argv[1], &cfg->cache_enabled);
    else if (IS("cache_size") && one) ok = parse_size(argv[1], &cfg->cache_max_bytes);
    else if (IS("cache_max_object") && one) ok = parse_size(argv[1], &cfg->cache_max_object);
    else if (IS("cache_ttl") && one) ok = parse_int(argv[1], 0, 86400 * 365, &cfg->cache_default_ttl);
    else if (IS("status_page") && one) ok = parse_bool(argv[1], &cfg->status_page);
    else if (IS("access_log") && one) ok = parse_bool(argv[1], &cfg->access_log);
    else if (IS("cgi") && argc == 3) {
        ok = copy_str(cfg->cgi_prefix, sizeof(cfg->cgi_prefix), argv[1]);
        if (ok == 0)
            ok = copy_str(cfg->cgi_dir, sizeof(cfg->cgi_dir), argv[2]);
        if (ok == 0 && argv[1][0] != '/')
            ok = -1;
        cfg->cgi_enabled = (ok == 0);
    } else if (IS("upstream") && argc >= 3) {
        return router_add_upstream(&cfg->router, argv[1], argv + 2, (size_t)(argc - 2), err,
                                   err_cap);
    } else if (IS("route") && (argc == 3 || (argc == 4 && !strcmp(argv[3], "strip")))) {
        return router_add_route(&cfg->router, ROUTE_PREFIX, argv[1], argv[2], argc == 4, err,
                                err_cap);
    } else if (IS("route_host") && argc == 3) {
        return router_add_route(&cfg->router, ROUTE_HOST, argv[1], argv[2], 0, err, err_cap);
    } else {
        snprintf(err, err_cap, "unknown directive or wrong argument count: '%s'", d);
        return -1;
    }
#undef IS
    if (ok < 0)
        snprintf(err, err_cap, "invalid value for '%s'", d);
    return ok;
}

int config_load_file(config_t *cfg, const char *path, char *err, size_t err_cap) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, err_cap, "%s: %s", path, strerror(errno));
        return -1;
    }
    char line[2048];
    char lerr[256];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(err, err_cap, "%s:%d: line too long", path, lineno);
            rc = -1;
            break;
        }
        if (config_apply_line(cfg, line, lerr, sizeof(lerr)) < 0) {
            snprintf(err, err_cap, "%s:%d: %s", path, lineno, lerr);
            rc = -1;
            break;
        }
    }
    fclose(f);
    return rc;
}

void config_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -c, --config FILE         load a config file (applied first)\n"
            "  -p, --port PORT           listen port (default 8080)\n"
            "  -b, --bind ADDR           bind address (default 0.0.0.0)\n"
            "  -r, --root DIR            static file root (default ./www)\n"
            "  -t, --threads N           worker threads (default 8)\n"
            "      --cgi PREFIX=DIR      run executables in DIR for URLs under PREFIX\n"
            "  -u, --upstream NAME=H:P[,H:P...]  define a backend pool\n"
            "      --route PREFIX=NAME   reverse-proxy PREFIX to pool NAME\n"
            "      --route-strip PREFIX=NAME  same, removing PREFIX from the path\n"
            "      --route-host HOST=NAME     reverse-proxy by Host header\n"
            "      --forward-proxy       accept absolute-form / CONNECT proxy requests\n"
            "      --cache               cache forward-proxy GET responses\n"
            "  -s, --set 'DIRECTIVE ARGS'  any config-file directive\n"
            "  -q, --quiet               disable the access log\n"
            "  -h, --help                show this help\n",
            prog);
}

/* Turn "a=b,c" into the config line "<directive> a b c". */
static int apply_kv(config_t *cfg, const char *directive, const char *kv, const char *suffix,
                    char *err, size_t err_cap) {
    char line[1024];
    int n = snprintf(line, sizeof(line), "%s %s %s", directive, kv, suffix);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        snprintf(err, err_cap, "%s: argument too long", directive);
        return -1;
    }
    char *eq = strchr(line + strlen(directive) + 1, '=');
    if (!eq) {
        snprintf(err, err_cap, "--%s expects KEY=VALUE", directive);
        return -1;
    }
    for (char *p = eq; *p; p++)
        if (*p == '=' || *p == ',')
            *p = ' ';
    return config_apply_line(cfg, line, err, err_cap);
}

int config_from_args(config_t *cfg, int argc, char **argv, char *err, size_t err_cap) {
    err[0] = '\0';
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config"))
            if (config_load_file(cfg, argv[i + 1], err, err_cap) < 0)
                return -1;
    }
    enum { OPT_CGI = 256, OPT_ROUTE, OPT_ROUTE_STRIP, OPT_ROUTE_HOST, OPT_FWD, OPT_CACHE };
    static const struct option opts[] = {
        {"config", required_argument, NULL, 'c'},   {"port", required_argument, NULL, 'p'},
        {"bind", required_argument, NULL, 'b'},     {"root", required_argument, NULL, 'r'},
        {"threads", required_argument, NULL, 't'},  {"upstream", required_argument, NULL, 'u'},
        {"set", required_argument, NULL, 's'},      {"quiet", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},           {"cgi", required_argument, NULL, OPT_CGI},
        {"route", required_argument, NULL, OPT_ROUTE},
        {"route-strip", required_argument, NULL, OPT_ROUTE_STRIP},
        {"route-host", required_argument, NULL, OPT_ROUTE_HOST},
        {"forward-proxy", no_argument, NULL, OPT_FWD},
        {"cache", no_argument, NULL, OPT_CACHE},    {NULL, 0, NULL, 0},
    };
    /* Routes must see every upstream, so they are applied after all other options. */
    char *deferred[MAX_ROUTES];
    const char *deferred_suffix[MAX_ROUTES];
    const char *deferred_dir[MAX_ROUTES];
    int ndeferred = 0, rc = 0, c;
    char line[1024];
    opterr = 0;
#if defined(__GLIBC__)
    optind = 0; /* glibc: 0 forces a full re-initialisation */
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    optreset = 1;
    optind = 1;
#else
    optind = 1;
#endif
    while (rc == 0 && (c = getopt_long(argc, argv, ":c:p:b:r:t:u:s:qh", opts, NULL)) != -1) {
        switch (c) {
        case 'c': break;
        case 'p': snprintf(line, sizeof(line), "listen %s", optarg); rc = config_apply_line(cfg, line, err, err_cap); break;
        case 'b': rc = copy_str(cfg->bind_addr, sizeof(cfg->bind_addr), optarg); break;
        case 'r': rc = copy_str(cfg->root, sizeof(cfg->root), optarg); break;
        case 't': snprintf(line, sizeof(line), "threads %s", optarg); rc = config_apply_line(cfg, line, err, err_cap); break;
        case 'u': rc = apply_kv(cfg, "upstream", optarg, "", err, err_cap); break;
        case 's': snprintf(line, sizeof(line), "%s", optarg); rc = config_apply_line(cfg, line, err, err_cap); break;
        case 'q': cfg->access_log = 0; break;
        case 'h': config_usage(argv[0]); return 1;
        case OPT_CGI: rc = apply_kv(cfg, "cgi", optarg, "", err, err_cap); break;
        case OPT_FWD: cfg->forward_proxy = 1; break;
        case OPT_CACHE: cfg->cache_enabled = 1; break;
        case OPT_ROUTE: case OPT_ROUTE_STRIP: case OPT_ROUTE_HOST:
            if (ndeferred == MAX_ROUTES) { snprintf(err, err_cap, "too many routes"); return -1; }
            deferred[ndeferred] = optarg;
            deferred_dir[ndeferred] = c == OPT_ROUTE_HOST ? "route_host" : "route";
            deferred_suffix[ndeferred++] = c == OPT_ROUTE_STRIP ? "strip" : "";
            break;
        case ':': snprintf(err, err_cap, "option '%s' needs an argument", argv[optind - 1]); return -1;
        default: snprintf(err, err_cap, "unknown option '%s'", argv[optind - 1]); return -1;
        }
        if (rc < 0 && !err[0])
            snprintf(err, err_cap, "invalid value for -%c", c);
    }
    if (rc == 0 && optind < argc) {
        snprintf(err, err_cap, "unexpected argument '%s'", argv[optind]);
        return -1;
    }
    for (int i = 0; rc == 0 && i < ndeferred; i++)
        rc = apply_kv(cfg, deferred_dir[i], deferred[i], deferred_suffix[i], err, err_cap);
    return rc < 0 ? -1 : 0;
}

int config_finalize(config_t *cfg, char *err, size_t err_cap) {
    struct stat st;
    if (!realpath(cfg->root, cfg->root_real) || stat(cfg->root_real, &st) < 0 ||
        !S_ISDIR(st.st_mode)) {
        snprintf(err, err_cap, "root '%s' is not a directory", cfg->root);
        return -1;
    }
    if (cfg->cgi_enabled) {
        char real[PATH_MAX];
        if (!realpath(cfg->cgi_dir, real) || stat(real, &st) < 0 || !S_ISDIR(st.st_mode)) {
            snprintf(err, err_cap, "cgi dir '%s' is not a directory", cfg->cgi_dir);
            return -1;
        }
        snprintf(cfg->cgi_dir, sizeof(cfg->cgi_dir), "%s", real);
        size_t n = strlen(cfg->cgi_prefix);
        while (n > 1 && cfg->cgi_prefix[n - 1] == '/')
            cfg->cgi_prefix[--n] = '\0';
    }
    if (strchr(cfg->index_file, '/')) {
        snprintf(err, err_cap, "index must be a file name");
        return -1;
    }
    if (cfg->cache_max_object > cfg->cache_max_bytes)
        cfg->cache_max_object = cfg->cache_max_bytes;
    return 0;
}
