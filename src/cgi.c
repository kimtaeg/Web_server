#include "cgi.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "netutil.h"
#include "response.h"

#define CGI_MAX_ENV (HTTP_MAX_HEADERS + 24)

int cgi_parse_output(const char *out, size_t len, int *status, char *content_type,
                     size_t ctype_cap, buf_t *extra_headers, size_t *body_off) {
    *status = 200;
    content_type[0] = '\0';
    int has_status = 0, has_location = 0;
    size_t pos = 0;
    for (;;) {
        const char *nl = memchr(out + pos, '\n', len - pos);
        if (!nl)
            return -1; /* header block never ended */
        size_t line_end = (size_t)(nl - out);
        size_t l = line_end;
        if (l > pos && out[l - 1] == '\r')
            l--;
        if (l == pos) {
            *body_off = line_end + 1;
            break;
        }
        const char *line = out + pos;
        size_t llen = l - pos;
        const char *colon = memchr(line, ':', llen);
        if (!colon || colon == line)
            return -1;
        size_t nlen = (size_t)(colon - line);
        for (size_t i = 0; i < nlen; i++)
            if (!isalnum((unsigned char)line[i]) && line[i] != '-' && line[i] != '_')
                return -1;
        const char *v = colon + 1;
        const char *vend = line + llen;
        while (v < vend && (*v == ' ' || *v == '\t'))
            v++;
        for (const char *c = v; c < vend; c++)
            if ((unsigned char)*c < 0x20 && *c != '\t')
                return -1;
        size_t vlen = (size_t)(vend - v);
#define NAME_IS(s) (nlen == strlen(s) && strncasecmp(line, s, nlen) == 0)
        if (NAME_IS("Status")) {
            if (vlen < 3 || !isdigit((unsigned char)v[0]) || !isdigit((unsigned char)v[1]) ||
                !isdigit((unsigned char)v[2]))
                return -1;
            *status = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
            if (*status < 100 || *status > 599)
                return -1;
            has_status = 1;
        } else if (NAME_IS("Content-Type")) {
            if (vlen >= ctype_cap)
                return -1;
            memcpy(content_type, v, vlen);
            content_type[vlen] = '\0';
        } else if (NAME_IS("Connection") || NAME_IS("Content-Length") ||
                   NAME_IS("Transfer-Encoding") || NAME_IS("Keep-Alive") ||
                   NAME_IS("Server") || NAME_IS("Date")) {
            /* the server owns framing and these headers */
        } else {
            if (NAME_IS("Location"))
                has_location = 1;
            buf_append(extra_headers, line, nlen);
            buf_append_str(extra_headers, ": ");
            buf_append(extra_headers, v, vlen);
            buf_append_str(extra_headers, "\r\n");
        }
#undef NAME_IS
        pos = line_end + 1;
    }
    if (has_location && !has_status)
        *status = 302;
    if (!content_type[0])
        snprintf(content_type, ctype_cap, "text/plain; charset=utf-8");
    return 0;
}

static int env_add(char **env, int *n, const char *fmt, ...) {
    if (*n >= CGI_MAX_ENV)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = malloc((size_t)len + 1);
    if (!s)
        return -1;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)len + 1, fmt, ap);
    va_end(ap);
    env[(*n)++] = s;
    env[*n] = NULL;
    return 0;
}

char **cgi_build_env(const req_ctx_t *ctx, const char *script_name, const char *path_info) {
    const http_request_t *req = ctx->req;
    char **env = calloc(CGI_MAX_ENV + 1, sizeof(char *));
    if (!env)
        return NULL;
    int n = 0;
    const char *ctype = http_request_header(req, "Content-Type");
    const char *host = http_request_header(req, "Host");
    int rc = 0;
    rc |= env_add(env, &n, "GATEWAY_INTERFACE=CGI/1.1");
    rc |= env_add(env, &n, "SERVER_SOFTWARE=%s", SERVER_NAME);
    rc |= env_add(env, &n, "SERVER_PROTOCOL=HTTP/1.%d", req->version_minor);
    rc |= env_add(env, &n, "SERVER_PORT=%d", ctx->srv->cfg->port);
    rc |= env_add(env, &n, "SERVER_NAME=%s", host ? host : "localhost");
    rc |= env_add(env, &n, "REQUEST_METHOD=%s", req->method_str);
    rc |= env_add(env, &n, "REQUEST_URI=%s", req->target);
    rc |= env_add(env, &n, "SCRIPT_NAME=%s", script_name);
    rc |= env_add(env, &n, "PATH_INFO=%s", path_info);
    rc |= env_add(env, &n, "QUERY_STRING=%s", req->query);
    rc |= env_add(env, &n, "REMOTE_ADDR=%s", ctx->conn->peer);
    rc |= env_add(env, &n, "PATH=/usr/local/bin:/usr/bin:/bin");
    if (req->body_len || req->method == HTTP_POST)
        rc |= env_add(env, &n, "CONTENT_LENGTH=%zu", req->body_len);
    if (ctype)
        rc |= env_add(env, &n, "CONTENT_TYPE=%s", ctype);
    for (size_t i = 0; i < req->header_count && rc == 0; i++) {
        const char *name = req->headers[i].name;
        if (!strcasecmp(name, "Content-Type") || !strcasecmp(name, "Content-Length") ||
            !strcasecmp(name, "Proxy")) /* "httpoxy": never let a client set HTTP_PROXY */
            continue;
        char key[128];
        size_t k = 0;
        for (const char *p = name; *p && k + 1 < sizeof(key); p++)
            key[k++] = *p == '-' ? '_' : (char)toupper((unsigned char)*p);
        key[k] = '\0';
        rc |= env_add(env, &n, "HTTP_%s=%s", key, req->headers[i].value);
    }
    if (rc) {
        cgi_free_env(env);
        return NULL;
    }
    return env;
}

void cgi_free_env(char **env) {
    if (!env)
        return;
    for (char **e = env; *e; e++)
        free(*e);
    free(env);
}

/* Feed the body to the child and collect stdout until EOF or the deadline. */
static int exchange(int in_fd, int out_fd, const char *body, size_t body_len, buf_t *out,
                    long long deadline) {
    size_t written = 0;
    if (body_len == 0) {
        close(in_fd);
        in_fd = -1;
    }
    char chunk[16384];
    for (;;) {
        struct pollfd p[2];
        int np = 0, out_idx, in_idx = -1;
        out_idx = np;
        p[np++] = (struct pollfd){.fd = out_fd, .events = POLLIN};
        if (in_fd >= 0) {
            in_idx = np;
            p[np++] = (struct pollfd){.fd = in_fd, .events = POLLOUT};
        }
        int left = (int)(deadline - now_ms());
        if (left <= 0) {
            if (in_fd >= 0)
                close(in_fd);
            return -2;
        }
        int r = poll(p, (nfds_t)np, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            break;
        if (in_idx >= 0 && p[in_idx].revents) {
            ssize_t w = write(in_fd, body + written, body_len - written);
            if (w > 0)
                written += (size_t)w;
            if (w < 0 && errno != EAGAIN && errno != EINTR)
                written = body_len; /* child stopped reading (EPIPE): give up on stdin */
            if (written == body_len) {
                close(in_fd);
                in_fd = -1;
            }
        }
        if (p[out_idx].revents) {
            ssize_t n = read(out_fd, chunk, sizeof(chunk));
            if (n == 0)
                break;
            if (n < 0 && errno != EAGAIN && errno != EINTR)
                break;
            if (n > 0 && (out->len + (size_t)n > CGI_MAX_OUTPUT || buf_append(out, chunk, (size_t)n) < 0)) {
                if (in_fd >= 0)
                    close(in_fd);
                return -1;
            }
        }
    }
    if (in_fd >= 0)
        close(in_fd);
    return 0;
}

static int run_script(req_ctx_t *ctx, const char *script_path, char **env, buf_t *out,
                      int *exit_status) {
    const config_t *cfg = ctx->srv->cfg;
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0)
        return -3;
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -3;
    }
    for (int i = 0; i < 2; i++) {
        set_cloexec(in_pipe[i]);
        set_cloexec(out_pipe[i]);
    }
    /* argv/envp are built before fork: after fork in a threaded process the child may only
     * call async-signal-safe functions until execve. */
    char *const argv[] = {(char *)script_path, NULL};
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        return -3;
    }
    if (pid == 0) {
        setpgid(0, 0); /* own process group, so a timeout kills grandchildren too */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        if (chdir(cfg->cgi_dir) < 0)
            _exit(126);
        /* Ignored signals and the blocked mask survive execve: undo the worker's SIGPIPE
         * ignore and its SIGINT/SIGTERM block so the script gets normal signal behaviour. */
        signal(SIGPIPE, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execve(script_path, argv, env);
        _exit(127);
    }
    setpgid(pid, pid); /* also from the parent: whichever runs first wins the race */
    close(in_pipe[0]);
    close(out_pipe[1]);
    set_nonblocking(in_pipe[1], 1);
    set_nonblocking(out_pipe[0], 1);

    int rc = exchange(in_pipe[1], out_pipe[0], ctx->req->body, ctx->req->body_len, out,
                      now_ms() + cfg->cgi_timeout_ms);
    close(out_pipe[0]);
    if (rc < 0)
        kill(-pid, SIGKILL); /* timeout (-2) or runaway output (-1): the whole group */
    int wst = 0;
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) {
    }
    *exit_status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst);
    return rc;
}

int cgi_handle(req_ctx_t *ctx) {
    const config_t *cfg = ctx->srv->cfg;
    http_request_t *req = ctx->req;
    ctx->handler = "cgi";
    if (req->method != HTTP_GET && req->method != HTTP_HEAD && req->method != HTTP_POST)
        return respond_error(ctx, 405, "Allow: GET, HEAD, POST\r\n");

    const char *rest = req->path + strlen(cfg->cgi_prefix);
    if (strcmp(cfg->cgi_prefix, "/") == 0)
        rest = req->path;
    if (rest[0] != '/' || rest[1] == '\0')
        return respond_error(ctx, 403, NULL);
    const char *name = rest + 1;
    size_t name_len = strcspn(name, "/");
    if (name[0] == '.' || name_len >= 256)
        return respond_error(ctx, 403, NULL);
    char script_name[512], script_path[PATH_MAX];
    snprintf(script_name, sizeof(script_name), "%s/%.*s",
             strcmp(cfg->cgi_prefix, "/") ? cfg->cgi_prefix : "", (int)name_len, name);
    int n = snprintf(script_path, sizeof(script_path), "%s/%.*s", cfg->cgi_dir, (int)name_len,
                     name);
    if (n < 0 || (size_t)n >= sizeof(script_path))
        return respond_error(ctx, 414, NULL);
    const char *path_info = name + name_len;

    struct stat st;
    if (stat(script_path, &st) < 0)
        return respond_error(ctx, errno == ENOENT || errno == ENOTDIR ? 404 : 403, NULL);
    if (!S_ISREG(st.st_mode) || access(script_path, X_OK) < 0)
        return respond_error(ctx, 403, NULL);

    char **env = cgi_build_env(ctx, script_name, path_info);
    if (!env)
        return respond_error(ctx, 500, NULL);
    buf_t out;
    buf_init(&out);
    int exit_status = 0;
    int rc = run_script(ctx, script_path, env, &out, &exit_status);
    cgi_free_env(env);

    int result;
    if (rc == -2) {
        log_error("cgi %s timed out after %d ms", script_name, cfg->cgi_timeout_ms);
        result = respond_error(ctx, 504, NULL);
    } else if (rc == -1) {
        log_error("cgi %s: output exceeds %u bytes", script_name, CGI_MAX_OUTPUT);
        result = respond_error(ctx, 502, NULL);
    } else if (rc < 0) {
        result = respond_error(ctx, 500, NULL);
    } else {
        int status;
        char ctype[256];
        buf_t extra;
        buf_init(&extra);
        size_t body_off = 0;
        if (out.len == 0 || cgi_parse_output(out.data, out.len, &status, ctype, sizeof(ctype),
                                             &extra, &body_off) < 0) {
            log_error("cgi %s: bad output (exit %d, %zu bytes)", script_name, exit_status,
                      out.len);
            result = respond_error(ctx, 502, NULL);
        } else {
            result = respond(ctx, status, ctype, out.data + body_off, out.len - body_off,
                             extra.len ? extra.data : NULL);
        }
        buf_free(&extra);
    }
    buf_free(&out);
    return result;
}
