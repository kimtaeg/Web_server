#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <sys/types.h>

long long now_sec(void);
long long now_ms(void); /* monotonic */
long long now_us(void); /* monotonic */

int set_nonblocking(int fd, int on);
int set_cloexec(int fd);
int set_io_timeouts(int fd, int timeout_ms);
/* Accepted/connected sockets: no SIGPIPE (macOS), TCP_NODELAY, close-on-exec. */
void socket_tune(int fd);

/* Writes everything or fails; never raises SIGPIPE. */
int send_all(int fd, const void *buf, size_t len);
ssize_t recv_some(int fd, void *buf, size_t cap);
/* 1 = ready, 0 = timeout, -1 = error. */
int wait_fd(int fd, short events, int timeout_ms);

/* Resolves host and connects with a deadline. Returns a blocking fd or -1 (errno set,
 * ETIMEDOUT on timeout). */
int connect_with_timeout(const char *host, int port, int timeout_ms);

int listen_tcp(const char *addr, int port, int backlog, char *err, size_t err_cap);

#endif
