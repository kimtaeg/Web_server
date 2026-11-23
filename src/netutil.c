#include "netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set per socket in socket_tune() */
#endif

long long now_sec(void) {
    return (long long)time(NULL);
}

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int set_nonblocking(int fd, int on) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int set_io_timeouts(int fd, int timeout_ms) {
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void socket_tune(int fd) {
    int one = 1;
    set_cloexec(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* EAGAIN here means SO_SNDTIMEO expired */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

ssize_t recv_some(int fd, void *buf, size_t cap) {
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

int wait_fd(int fd, short events, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = events};
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left < 0)
            left = 0;
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r;
        return 1;
    }
}

static int connect_addr(const struct addrinfo *ai, int timeout_ms) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
        return -1;
    socket_tune(fd);
    set_nonblocking(fd, 1);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS)
        goto fail;
    if (rc < 0) {
        int w = wait_fd(fd, POLLOUT, timeout_ms);
        if (w == 0) {
            errno = ETIMEDOUT;
            goto fail;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (w < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
            goto fail;
        if (soerr) {
            errno = soerr;
            goto fail;
        }
    }
    set_nonblocking(fd, 0);
    return fd;
fail:;
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int connect_with_timeout(const char *host, int port, int timeout_ms) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next)
        fd = connect_addr(ai, timeout_ms);
    int saved = errno;
    freeaddrinfo(res);
    errno = saved;
    return fd;
}

int listen_tcp(const char *addr, int port, int backlog, char *err, size_t err_cap) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    int gai = getaddrinfo(addr, portstr, &hints, &res);
    if (gai != 0) {
        snprintf(err, err_cap, "bad bind address '%s': %s", addr, gai_strerror(gai));
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        snprintf(err, err_cap, "socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }
    /* Lets a restarted server bind while old connections sit in TIME_WAIT. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_cloexec(fd);
    if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
        snprintf(err, err_cap, "bind %s:%d: %s", addr, port, strerror(errno));
        goto fail;
    }
    if (listen(fd, backlog) < 0) {
        snprintf(err, err_cap, "listen: %s", strerror(errno));
        goto fail;
    }
    if (set_nonblocking(fd, 1) < 0) {
        snprintf(err, err_cap, "fcntl: %s", strerror(errno));
        goto fail;
    }
    freeaddrinfo(res);
    return fd;
fail:
    close(fd);
    freeaddrinfo(res);
    return -1;
}
