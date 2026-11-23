#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "static_file.h"
#include "test.h"

static config_t cfg;

static int apply(const char *line) {
    char buf[512], err[256];
    snprintf(buf, sizeof(buf), "%s", line);
    return config_apply_line(&cfg, buf, err, sizeof(err));
}

static void test_directives(void) {
    config_defaults(&cfg);
    CHECK_INT(cfg.port, 8080);
    CHECK_INT(apply("listen 9090   # trailing comment"), 0);
    CHECK_INT(cfg.port, 9090);
    CHECK_INT(apply("listen 0.0.0.0:7070"), 0);
    CHECK_INT(cfg.port, 7070);
    CHECK_STR(cfg.bind_addr, "0.0.0.0");
    CHECK_INT(apply("   # only a comment"), 0);
    CHECK_INT(apply(""), 0);
    CHECK_INT(apply("threads 16"), 0);
    CHECK_INT(cfg.threads, 16);
    CHECK_INT(apply("max_body_size 2m"), 0);
    CHECK_INT(cfg.max_body, 2 * 1024 * 1024);
    CHECK_INT(apply("cache on"), 0);
    CHECK_INT(cfg.cache_enabled, 1);
    CHECK_INT(apply("forward_proxy off"), 0);
    CHECK_INT(cfg.forward_proxy, 0);
    CHECK_INT(apply("cgi /cgi ./cgi-bin"), 0);
    CHECK_INT(cfg.cgi_enabled, 1);
    CHECK_INT(apply("upstream api 127.0.0.1:1 127.0.0.1:2"), 0);
    CHECK_INT(apply("route /api api strip"), 0);
    CHECK_INT(apply("route_host a.test api"), 0);
    CHECK_INT(cfg.router.n_routes, 2);
    CHECK_INT(cfg.router.routes[0].strip_prefix, 1);

    CHECK_INT(apply("listen 0"), -1);
    CHECK_INT(apply("listen 65536"), -1);
    CHECK_INT(apply("threads lots"), -1);
    CHECK_INT(apply("threads 0"), -1);
    CHECK_INT(apply("cache maybe"), -1);
    CHECK_INT(apply("route /x missing"), -1);
    CHECK_INT(apply("route /x api sideways"), -1);
    CHECK_INT(apply("no_such_directive 1"), -1);
    CHECK_INT(apply("root"), -1);
    CHECK_INT(apply("cgi relative ./dir"), -1);

    size_t v;
    CHECK_INT(parse_size("512", &v), 0);
    CHECK_INT(v, 512);
    CHECK_INT(parse_size("4k", &v), 0);
    CHECK_INT(v, 4096);
    CHECK_INT(parse_size("1MB", &v), 0);
    CHECK_INT(v, 1048576);
    CHECK_INT(parse_size("1x", &v), -1);
    CHECK_INT(parse_size("-1", &v), -1);
    CHECK_INT(parse_size("", &v), -1);
}

static void test_args(void) {
    config_defaults(&cfg);
    char err[256];
    char *argv[] = {"webserv", "-p", "8181", "-r", "www", "-t", "4", "-q",
                    "--route-strip", "/api=pool", "-u", "pool=127.0.0.1:1,127.0.0.1:2",
                    "--route-host", "h.test=pool", "--cgi", "/cgi-bin=cgi-bin",
                    "--forward-proxy", "--cache", "-s", "cache_ttl 5", NULL};
    int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;
    CHECK_INT(config_from_args(&cfg, argc, argv, err, sizeof(err)), 0);
    CHECK_INT(cfg.port, 8181);
    CHECK_INT(cfg.threads, 4);
    CHECK_INT(cfg.access_log, 0);
    CHECK_INT(cfg.router.n_upstreams, 1);
    CHECK_INT(cfg.router.upstreams[0].count, 2);
    CHECK_INT(cfg.router.n_routes, 2); /* routes given before -u still resolve */
    CHECK_INT(cfg.forward_proxy, 1);
    CHECK_INT(cfg.cache_enabled, 1);
    CHECK_INT(cfg.cache_default_ttl, 5);
    CHECK_STR(cfg.cgi_prefix, "/cgi-bin");

    config_defaults(&cfg);
    char *bad[] = {"webserv", "--bogus", NULL};
    CHECK_INT(config_from_args(&cfg, 2, bad, err, sizeof(err)), -1);
    char *missing[] = {"webserv", "-p", NULL};
    CHECK_INT(config_from_args(&cfg, 2, missing, err, sizeof(err)), -1);
    char *badfile[] = {"webserv", "-c", "/nonexistent/conf", NULL};
    CHECK_INT(config_from_args(&cfg, 3, badfile, err, sizeof(err)), -1);
}

static char tmp_root[PATH_MAX], tmp_real[PATH_MAX];

static void write_file(const char *rel, const char *content, mode_t mode) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", tmp_root, rel);
    FILE *f = fopen(p, "w");
    fputs(content, f);
    fclose(f);
    chmod(p, mode);
}

static int resolve(const char *url, char *out) {
    struct stat st;
    char buf[PATH_MAX];
    return static_resolve(tmp_real, "index.html", url, out ? out : buf, PATH_MAX, &st);
}

static void test_static_resolve(void) {
    snprintf(tmp_root, sizeof(tmp_root), "/tmp/cwebserv-unit-XXXXXX");
    if (!mkdtemp(tmp_root)) {
        CHECK(!"mkdtemp failed");
        return;
    }
    realpath(tmp_root, tmp_real);
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/dir", tmp_root);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/empty", tmp_root);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.well-known", tmp_root);
    mkdir(p, 0755);
    write_file("index.html", "<h1>root</h1>", 0644);
    write_file("dir/index.html", "dir", 0644);
    write_file("dir/a.txt", "a", 0644);
    write_file("secret.txt", "s", 0000);
    write_file(".env", "x", 0644);
    write_file(".well-known/security.txt", "w", 0644);
    snprintf(p, sizeof(p), "%s/escape", tmp_root);
    symlink("/etc/hosts", p);
    snprintf(p, sizeof(p), "%s/inside", tmp_root);
    symlink("dir/a.txt", p);

    char out[PATH_MAX], want[PATH_MAX];
    CHECK_INT(resolve("/", out), 0);
    snprintf(want, sizeof(want), "%s/index.html", tmp_real);
    CHECK_STR(out, want);
    CHECK_INT(resolve("/dir/a.txt", out), 0);
    CHECK_INT(resolve("/dir/", out), 0);
    CHECK_INT(resolve("/dir", NULL), 301);
    CHECK_INT(resolve("/empty/", NULL), 403);
    CHECK_INT(resolve("/missing.html", NULL), 404);
    CHECK_INT(resolve("/dir/a.txt/x", NULL), 404);
    CHECK_INT(resolve("/.env", NULL), 403);
    CHECK_INT(resolve("/dir/.git/config", NULL), 403);
    CHECK_INT(resolve("/.well-known/security.txt", NULL), 0);
    CHECK_INT(resolve("/escape", NULL), 403);
    CHECK_INT(resolve("/inside", out), 0);
    snprintf(want, sizeof(want), "%s/dir/a.txt", tmp_real);
    CHECK_STR(out, want);
    if (geteuid() != 0) { /* root can read mode 000 files */
        char ignored[PATH_MAX];
        struct stat st;
        int rc = static_resolve(tmp_real, "index.html", "/secret.txt", ignored, sizeof(ignored), &st);
        CHECK(rc == 0); /* resolvable; open() is what returns EACCES -> 403 */
    }
    CHECK_INT(resolve("relative", NULL), 403);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_root);
    if (system(cmd) != 0)
        CHECK(!"cleanup failed");
}

void suite_config(void) {
    test_directives();
    test_args();
}

void suite_static(void) {
    test_static_resolve();
}
