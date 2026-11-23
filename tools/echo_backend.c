/*
 * Tiny backend used by the functional tests and demos. It answers with JSON describing the
 * request it saw, plus a few special paths that misbehave on purpose:
 *   /slow?ms=N    wait N ms before answering (proxy timeout tests)
 *   /chunked      chunked response body
 *   /close        body delimited by connection close (no Content-Length)
 *   /cacheable    Cache-Control: max-age=30 and a per-backend hit counter
 *   /garbage      not an HTTP response at all (502 tests)
 */
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/buffer.h"
#include "../src/http.h"
#include "../src/netutil.h"

static const char *g_name = "backend";
static int g_port;
static _Atomic unsigned long g_hits;

static void json_escape(buf_t *b, const char *s) {
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_printf(b, "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            buf_printf(b, "\\u%04x", (unsigned char)*s);
        else
            buf_append(b, s, 1);
    }
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static int answer(int fd, const http_request_t *req) {
    buf_t out, body;
    buf_init(&out);
    buf_init(&body);
    int keep = req->keep_alive;
    unsigned long hits = atomic_fetch_add(&g_hits, 1) + 1;
    int rc = 0;

    if (strcmp(req->path, "/garbage") == 0) {
        rc = send_all(fd, "NOT HTTP AT ALL\r\n\r\n", 19);
        keep = 0;
        goto done;
    }
    if (strcmp(req->path, "/slow") == 0) {
        const char *ms = strstr(req->query, "ms=");
        sleep_ms(ms ? strtol(ms + 3, NULL, 10) : 3000);
    }
    if (strcmp(req->path, "/chunked") == 0) {
        buf_printf(&out, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n"
                         "6\r\nhello \r\n6\r\nworld!\r\n0\r\n\r\n",
                   keep ? "keep-alive" : "close");
        rc = send_all(fd, out.data, out.len);
        goto done;
    }
    buf_printf(&body, "{\"backend\": \"");
    json_escape(&body, g_name);
    buf_printf(&body, "\", \"port\": %d, \"method\": \"%s\", \"target\": \"", g_port,
               req->method_str);
    json_escape(&body, req->target);
    buf_printf(&body, "\", \"body_bytes\": %zu, \"hits\": %lu", req->body_len, hits);
    const char *names[] = {"Host", "X-Forwarded-For", "X-Forwarded-Host", "Via", "Connection"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *v = http_request_header(req, names[i]);
        buf_printf(&body, ", \"%s\": \"", names[i]);
        json_escape(&body, v ? v : "");
        buf_append_str(&body, "\"");
    }
    buf_append_str(&body, "}\n");

    if (strcmp(req->path, "/close") == 0) {
        buf_printf(&out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Connection: close\r\n\r\n");
        keep = 0;
    } else {
        buf_printf(&out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: %zu\r\nX-Backend: %s\r\n%sConnection: %s\r\n\r\n",
                   body.len, g_name,
                   strcmp(req->path, "/cacheable") == 0 ? "Cache-Control: max-age=30\r\n" : "",
                   keep ? "keep-alive" : "close");
    }
    if (req->method != HTTP_HEAD)
        buf_append(&out, body.data, body.len);
    rc = send_all(fd, out.data, out.len);
done:
    buf_free(&out);
    buf_free(&body);
    return rc == 0 && keep;
}

static void *serve(void *arg) {
    int fd = (int)(intptr_t)arg;
    buf_t in;
    buf_init(&in);
    char chunk[16384];
    for (;;) {
        http_request_t req;
        size_t used;
        int status;
        http_parse_result_t r =
            http_parse_request(in.data ? in.data : "", in.len, 64u << 20, &req, &used, &status);
        if (r == HTTP_PARSE_ERROR)
            break;
        if (r == HTTP_PARSE_INCOMPLETE) {
            ssize_t n = recv_some(fd, chunk, sizeof(chunk));
            if (n <= 0)
                break;
            buf_append(&in, chunk, (size_t)n);
            continue;
        }
        buf_consume(&in, used);
        int keep = answer(fd, &req);
        http_request_free(&req);
        if (!keep)
            break;
    }
    buf_free(&in);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s PORT [NAME]\n", argv[0]);
        return 2;
    }
    g_port = atoi(argv[1]);
    if (argc > 2)
        g_name = argv[2];
    signal(SIGPIPE, SIG_IGN);
    char err[256];
    int lfd = listen_tcp("127.0.0.1", g_port, 512, err, sizeof(err));
    if (lfd < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    set_nonblocking(lfd, 0);
    fprintf(stderr, "echo_backend %s listening on 127.0.0.1:%d\n", g_name, g_port);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            return 1;
        }
        socket_tune(fd);
        pthread_t t;
        if (pthread_create(&t, NULL, serve, (void *)(intptr_t)fd) != 0) {
            close(fd);
            continue;
        }
        pthread_detach(t);
    }
}
