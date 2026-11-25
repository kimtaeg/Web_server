/*
 * config.c — 설정 파일 파싱. 형식은 config.h 참고.
 */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

#define CONFIG_MAX_FILE (1024 * 1024)
#define MAX_TOKENS (CONFIG_MAX_BACKENDS + 4)

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->listen_host, sizeof c->listen_host, "0.0.0.0");
    c->listen_port = 8080;
    snprintf(c->root, sizeof c->root, "./www");
    c->workers = 8;
    c->queue = 64;
    c->idle_timeout_ms = 5000;
    c->header_timeout_ms = 10000;
    c->max_requests = 100;
    c->cgi_timeout_ms = 5000;
    c->upstream_connect_timeout_ms = 1000;
    c->upstream_read_timeout_ms = 5000;
    c->upstream_down_secs = 10;
    c->forward_proxy = 0; /* open proxy 방지: 명시적으로 켜야 한다 */
    c->cache_max_bytes = 64L * 1024 * 1024;
    c->cache_max_object_bytes = 1024L * 1024;
    c->cache_default_ttl_secs = 60;
    c->https_port = 8443; /* tls_cert/tls_key 가 있을 때만 연다 */
}

int config_parse_hostport(const char *s, config_hostport_t *out)
{
    const char *colon;
    const char *hs = s;
    size_t hl;
    char *end;
    long port;

    if (s[0] == '[') { /* [IPv6]:port */
        const char *rb = strchr(s, ']');

        if (rb == NULL || rb[1] != ':')
            return -1;
        hs = s + 1;
        hl = (size_t)(rb - hs);
        colon = rb + 1;
    } else {
        colon = strrchr(s, ':');
        if (colon == NULL || strchr(s, ':') != colon) /* 대괄호 없는 IPv6 는 모호 → 거부 */
            return -1;
        hl = (size_t)(colon - s);
    }
    if (hl == 0 || hl >= sizeof out->host)
        return -1;
    errno = 0;
    port = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port < 1 || port > 65535)
        return -1;
    memcpy(out->host, hs, hl);
    out->host[hl] = '\0';
    out->port = (int)port;
    return 0;
}

static int parse_num(const char *s, long min, long max, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static int copy_str(char *dst, size_t cap, const char *src)
{
    if (strlen(src) >= cap)
        return -1;
    memcpy(dst, src, strlen(src) + 1);
    return 0;
}

/* 숫자 하나를 받는 지시어 표 */
static const struct {
    const char *name;
    size_t off;
    long min;
    long max;
} num_directives[] = {
    {"workers", offsetof(config_t, workers), 1, 1024},
    {"queue", offsetof(config_t, queue), 1, 65536},
    {"idle_timeout_ms", offsetof(config_t, idle_timeout_ms), 1, 3600000},
    {"header_timeout_ms", offsetof(config_t, header_timeout_ms), 0, 3600000},
    {"max_requests", offsetof(config_t, max_requests), 1, 1000000},
    {"cgi_timeout_ms", offsetof(config_t, cgi_timeout_ms), 1, 3600000},
    {"upstream_connect_timeout_ms", offsetof(config_t, upstream_connect_timeout_ms), 1, 600000},
    {"upstream_read_timeout_ms", offsetof(config_t, upstream_read_timeout_ms), 1, 3600000},
    {"upstream_down_secs", offsetof(config_t, upstream_down_secs), 0, 86400},
    {"cache_max_bytes", offsetof(config_t, cache_max_bytes), 0, 16L * 1024 * 1024 * 1024},
    {"cache_max_object_bytes", offsetof(config_t, cache_max_object_bytes), 1, 1024L * 1024 * 1024},
    {"cache_default_ttl_secs", offsetof(config_t, cache_default_ttl_secs), 0, 365L * 86400},
    {"https_port", offsetof(config_t, https_port), 1, 65535},
};

/* 토큰들로 지시어 하나를 처리. 오류면 메시지를 쓰고 -1. */
static int apply(config_t *c, char **tok, int nt, char *msg, size_t msz)
{
    size_t i;

    for (i = 0; i < sizeof num_directives / sizeof num_directives[0]; i++) {
        if (strcmp(tok[0], num_directives[i].name) == 0) {
            long *dst = (long *)((char *)c + num_directives[i].off);

            if (nt != 2) {
                snprintf(msg, msz, "'%s' 는 값 하나가 필요하다", tok[0]);
                return -1;
            }
            if (parse_num(tok[1], num_directives[i].min, num_directives[i].max, dst) != 0) {
                snprintf(msg, msz, "'%s' 의 값 '%s' 가 잘못됨 (%ld~%ld)", tok[0], tok[1],
                         num_directives[i].min, num_directives[i].max);
                return -1;
            }
            return 0;
        }
    }

    if (strcmp(tok[0], "listen") == 0) {
        config_hostport_t hp;

        if (nt != 2 || config_parse_hostport(tok[1], &hp) != 0 ||
            copy_str(c->listen_host, sizeof c->listen_host, hp.host) != 0) {
            snprintf(msg, msz, "listen 은 'host:port' 하나가 필요하다");
            return -1;
        }
        c->listen_port = hp.port;
        return 0;
    }
    if (strcmp(tok[0], "forward_proxy") == 0) {
        if (nt != 2 || (strcmp(tok[1], "on") != 0 && strcmp(tok[1], "off") != 0)) {
            snprintf(msg, msz, "forward_proxy 는 on 또는 off");
            return -1;
        }
        c->forward_proxy = strcmp(tok[1], "on") == 0;
        return 0;
    }
    if (strcmp(tok[0], "root") == 0) {
        if (nt != 2 || copy_str(c->root, sizeof c->root, tok[1]) != 0) {
            snprintf(msg, msz, "root 는 경로 하나가 필요하다");
            return -1;
        }
        return 0;
    }
    if (strcmp(tok[0], "tls_cert") == 0 || strcmp(tok[0], "tls_key") == 0) {
        char *dst = tok[0][4] == 'c' ? c->tls_cert : c->tls_key;

        if (nt != 2 || copy_str(dst, PATH_MAX, tok[1]) != 0) {
            snprintf(msg, msz, "%s 는 경로 하나가 필요하다", tok[0]);
            return -1;
        }
        return 0;
    }
    if (strcmp(tok[0], "cgi") == 0) {
        if (nt != 3 || tok[1][0] != '/' ||
            copy_str(c->cgi_prefix, sizeof c->cgi_prefix, tok[1]) != 0 ||
            copy_str(c->cgi_dir, sizeof c->cgi_dir, tok[2]) != 0) {
            snprintf(msg, msz, "cgi 는 '/접두사 디렉터리' 가 필요하다");
            return -1;
        }
        return 0;
    }
    if (strcmp(tok[0], "route") == 0) {
        config_route_t *r;
        int k = 1;
        int b;

        if (c->nroutes >= CONFIG_MAX_ROUTES) {
            snprintf(msg, msz, "route 는 최대 %d 개", CONFIG_MAX_ROUTES);
            return -1;
        }
        r = &c->routes[c->nroutes];
        memset(r, 0, sizeof *r);
        if (k < nt && strncmp(tok[k], "host=", 5) == 0) {
            if (tok[k][5] == '\0' || copy_str(r->host, sizeof r->host, tok[k] + 5) != 0) {
                snprintf(msg, msz, "route 의 host= 값이 잘못됨");
                return -1;
            }
            k++;
        }
        if (k >= nt || tok[k][0] != '/' || copy_str(r->prefix, sizeof r->prefix, tok[k]) != 0) {
            snprintf(msg, msz, "route 는 '/' 로 시작하는 접두사가 필요하다");
            return -1;
        }
        k++;
        if (k >= nt) {
            snprintf(msg, msz, "route %s 에 백엔드(host:port)가 없다", r->prefix);
            return -1;
        }
        for (b = k; b < nt; b++) {
            if (r->nbackends >= CONFIG_MAX_BACKENDS) {
                snprintf(msg, msz, "route 당 백엔드는 최대 %d 개", CONFIG_MAX_BACKENDS);
                return -1;
            }
            if (config_parse_hostport(tok[b], &r->backends[r->nbackends]) != 0) {
                snprintf(msg, msz, "백엔드 주소 '%s' 가 잘못됨 (host:port)", tok[b]);
                return -1;
            }
            r->nbackends++;
        }
        c->nroutes++;
        return 0;
    }
    snprintf(msg, msz, "알 수 없는 지시어 '%s'", tok[0]);
    return -1;
}

int config_parse_string(const char *text, config_t *c, char *err, size_t errsz)
{
    const char *p = text;
    int lineno = 0;

    while (*p != '\0') {
        char line[4096];
        char *tok[MAX_TOKENS];
        char *s;
        char *save = NULL;
        char msg[512];
        int nt = 0;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        lineno++;
        if (len >= sizeof line) {
            snprintf(err, errsz, "line %d: 줄이 너무 길다", lineno);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p += len + (nl ? 1 : 0);

        s = strchr(line, '#');
        if (s != NULL)
            *s = '\0';
        for (s = strtok_r(line, " \t\r", &save); s != NULL; s = strtok_r(NULL, " \t\r", &save)) {
            if (nt >= MAX_TOKENS) {
                snprintf(err, errsz, "line %d: 인자가 너무 많다", lineno);
                return -1;
            }
            tok[nt++] = s;
        }
        if (nt == 0)
            continue;
        if (apply(c, tok, nt, msg, sizeof msg) != 0) {
            snprintf(err, errsz, "line %d: %s", lineno, msg);
            return -1;
        }
    }
    return 0;
}

int config_load(const char *path, config_t *c, char *err, size_t errsz)
{
    FILE *f = NULL;
    buf_t b;
    char chunk[4096];
    size_t n;
    int rc = -1;

    buf_init(&b, 4096, CONFIG_MAX_FILE);
    f = fopen(path, "r");
    if (f == NULL) {
        snprintf(err, errsz, "'%s' 를 열 수 없음: %s", path, strerror(errno));
        goto cleanup;
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (buf_append(&b, chunk, n) != BUF_OK) {
            snprintf(err, errsz, "'%s' 가 너무 크다 (최대 %d 바이트)", path, CONFIG_MAX_FILE);
            goto cleanup;
        }
    }
    if (ferror(f)) {
        snprintf(err, errsz, "'%s' 읽기 실패", path);
        goto cleanup;
    }
    if (memchr(b.data, '\0', b.len) != NULL) {
        snprintf(err, errsz, "'%s' 에 NUL 바이트가 있다", path);
        goto cleanup;
    }
    rc = config_parse_string(b.data, c, err, errsz);

cleanup:
    if (f != NULL)
        fclose(f);
    buf_free(&b);
    return rc;
}
