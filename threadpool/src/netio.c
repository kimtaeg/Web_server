/*
 * netio.c — 부분 전송·EINTR·타임아웃을 처리하는 소켓 I/O 헬퍼. 이유는 netio.h 참고.
 */
#include "netio.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "platform.h"

ssize_t send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    size_t off = 0;

    while (off < n) {
        ssize_t w = send(fd, c + off, n - off, PLATFORM_SEND_FLAGS);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* EAGAIN = SO_SNDTIMEO 만료, EPIPE/ECONNRESET = 상대가 끊음 */
        }
        off += (size_t)w;
    }
    return (ssize_t)n;
}

ssize_t recv_some(int fd, void *p, size_t n, int timeout_ms)
{
    struct pollfd pfd;
    ssize_t r;

    pfd.fd = fd;
    pfd.events = POLLIN;
    for (;;) {
        int pr;

        pfd.revents = 0;
        pr = poll(&pfd, 1, timeout_ms);
        if (pr > 0)
            break;
        if (pr == 0)
            return NETIO_TIMEOUT;
        if (errno != EINTR)
            return -1;
        /* EINTR: 같은 타임아웃으로 다시 기다린다. 시그널은 종료 시에만 오고,
         * 호출자(conn)가 짧은 간격으로 나눠 부르며 종료 플래그를 확인하므로 충분하다. */
    }
    /* POLLHUP/POLLERR 도 recv 로 확인하면 0(EOF) 또는 -1 로 정확히 구분된다. */
    do {
        r = recv(fd, p, n, 0);
    } while (r < 0 && errno == EINTR);
    return r;
}

ssize_t send_nowait(int fd, const void *p, size_t n)
{
    ssize_t w;
    int fl;

    /*
     * MSG_DONTWAIT 플래그는 쓰지 않는다: macOS 의 AF_UNIX 소켓에서는 무시되어 블록하는 것을
     * 테스트로 확인했다. 대신 POSIX 표준인 O_NONBLOCK 을 fd 에 건다.
     * 이 함수 뒤에는 호출자가 곧바로 close 하므로 fd 상태를 바꿔도 부작용이 없다.
     */
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    do {
        w = send(fd, p, n, PLATFORM_SEND_FLAGS);
    } while (w < 0 && errno == EINTR);
    return w;
}

int netio_set_send_timeout(int fd, int timeout_ms)
{
    struct timeval tv;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

int netio_set_recv_timeout(int fd, int timeout_ms)
{
    struct timeval tv;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
