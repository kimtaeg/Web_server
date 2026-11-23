#if defined(__APPLE__)
#undef _POSIX_C_SOURCE /* macOS only declares sendfile(2) outside strict POSIX mode */
#endif
#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/uio.h>
#endif

#include "mime.h"
#include "netutil.h"
#include "response.h"

#define SMALL_FILE_LIMIT (64 * 1024)

static int errno_to_status(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR: return 404;
    case EACCES:
    case EPERM:
    case ELOOP: return 403;
    case ENAMETOOLONG: return 414;
    default: return 500;
    }
}

/* Hidden files (.git, .env, .htpasswd ...) are never served. */
static int has_dot_segment(const char *path) {
    for (const char *p = path; (p = strchr(p, '/')) != NULL; p++)
        if (p[1] == '.' && strncmp(p + 1, ".well-known", 11) != 0)
            return 1;
    return 0;
}

static int inside_root(const char *root_real, const char *real) {
    size_t n = strlen(root_real);
    if (n == 1 && root_real[0] == '/')
        return 1;
    return strncmp(real, root_real, n) == 0 && (real[n] == '/' || real[n] == '\0');
}

int static_resolve(const char *root_real, const char *index_file, const char *url_path,
                   char *out, size_t out_cap, struct stat *st) {
    if (url_path[0] != '/')
        return 403;
    if (has_dot_segment(url_path))
        return 403;
    char joined[PATH_MAX];
    int is_dir_url = url_path[strlen(url_path) - 1] == '/';
    int n = snprintf(joined, sizeof(joined), "%s%s", root_real, url_path);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return 414;
    if (stat(joined, st) < 0)
        return errno_to_status(errno);
    if (S_ISDIR(st->st_mode)) {
        if (!is_dir_url)
            return 301;
        n = snprintf(joined, sizeof(joined), "%s%s%s", root_real, url_path, index_file);
        if (n < 0 || (size_t)n >= sizeof(joined))
            return 414;
        if (stat(joined, st) < 0)
            return errno == ENOENT ? 403 : errno_to_status(errno); /* no directory listing */
    }
    char real[PATH_MAX];
    if (!realpath(joined, real))
        return errno_to_status(errno);
    if (!inside_root(root_real, real)) /* a symlink pointing outside the root */
        return 403;
    if (stat(real, st) < 0)
        return errno_to_status(errno);
    if (!S_ISREG(st->st_mode))
        return 403;
    if (strlen(real) >= out_cap)
        return 414;
    memcpy(out, real, strlen(real) + 1);
    return 0;
}

static int send_file_body(req_ctx_t *ctx, int fd, off_t size) {
    int sock = ctx->conn->fd;
    off_t off = 0;
#if defined(__linux__)
    while (off < size) {
        ssize_t n = sendfile(sock, fd, &off, (size_t)(size - off));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        ctx->bytes += n;
    }
    return 0;
#elif defined(__APPLE__)
    while (off < size) {
        off_t len = size - off;
        int rc = sendfile(fd, sock, off, &len, NULL, 0);
        off += len;
        ctx->bytes += len;
        if (rc < 0 && errno != EINTR && !(errno == EAGAIN && len > 0))
            return -1;
        if (rc == 0 && len == 0)
            return -1;
    }
    return 0;
#else
    char chunk[64 * 1024];
    while (off < size) {
        ssize_t n = pread(fd, chunk, sizeof(chunk), off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || ctx_send(ctx, chunk, (size_t)n) < 0)
            return -1;
        off += n;
    }
    return 0;
#endif
}

static int redirect_to_dir(req_ctx_t *ctx) {
    buf_t loc;
    buf_init(&loc);
    buf_append_str(&loc, "Location: ");
    http_encode_path(&loc, ctx->req->path);
    buf_append_str(&loc, "/");
    if (ctx->req->query[0])
        buf_printf(&loc, "?%s", ctx->req->query);
    buf_append_str(&loc, "\r\n");
    int rc = respond_error(ctx, 301, loc.data);
    buf_free(&loc);
    return rc;
}

int static_serve(req_ctx_t *ctx) {
    const config_t *cfg = ctx->srv->cfg;
    http_request_t *req = ctx->req;
    ctx->handler = "static";
    if (req->method != HTTP_GET && req->method != HTTP_HEAD)
        return respond_error(ctx, 405, "Allow: GET, HEAD\r\n");

    char path[PATH_MAX];
    struct stat st;
    int rc = static_resolve(cfg->root_real, cfg->index_file, req->path, path, sizeof(path), &st);
    if (rc == 301)
        return redirect_to_dir(ctx);
    if (rc)
        return respond_error(ctx, rc, NULL);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return respond_error(ctx, errno_to_status(errno), NULL);
    set_cloexec(fd); /* CGI children must not inherit it */
    if (fstat(fd, &st) < 0) {
        close(fd);
        return respond_error(ctx, 500, NULL);
    }

    char etag[64], last_mod[64], extra[256];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)st.st_size,
             (unsigned long long)st.st_mtime);
    http_format_date((long long)st.st_mtime, last_mod, sizeof(last_mod));
    snprintf(extra, sizeof(extra), "Last-Modified: %s\r\nETag: %s\r\n", last_mod, etag);

    const char *inm = http_request_header(req, "If-None-Match");
    const char *ims = http_request_header(req, "If-Modified-Since");
    if ((inm && (strcmp(inm, etag) == 0 || strcmp(inm, "*") == 0)) ||
        (!inm && ims && strcmp(ims, last_mod) == 0)) {
        close(fd);
        return respond_head(ctx, 304, NULL, -1, extra);
    }

    const char *type = mime_type_for_path(path);
    if (st.st_size <= SMALL_FILE_LIMIT) {
        char small[SMALL_FILE_LIMIT];
        ssize_t got = 0;
        while (got < st.st_size) {
            ssize_t n = read(fd, small + got, (size_t)(st.st_size - got));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            got += n;
        }
        close(fd);
        if (got != st.st_size)
            return respond_error(ctx, 500, NULL);
        return respond(ctx, 200, type, small, (size_t)got, extra);
    }
    rc = respond_head(ctx, 200, type, (long long)st.st_size, extra);
    if (rc == 0 && req->method != HTTP_HEAD && send_file_body(ctx, fd, st.st_size) < 0) {
        ctx->keep_alive = 0;
        rc = -1;
    }
    close(fd);
    return rc;
}
