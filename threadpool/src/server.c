/*
 * server.c — listen 소켓과 accept 루프.
 */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "http_response.h"
#include "log.h"
#include "netio.h"
#include "platform.h"

/*
 * accept 전에 poll 로 기다리는 최대 시간. 종료 플래그 확인 주기다.
 * (시그널이 "플래그 확인 직후 ~ accept 진입 직전"에 오면 EINTR 이 나지 않고 accept 에서
 *  다음 연결까지 잠들 수 있다. poll 타임아웃이 이 경쟁 조건을 덮는다.)
 */
#define ACCEPT_POLL_MS 500

static const char *bind_hint(int e)
{
    switch (e) {
    case EADDRINUSE: return " (이미 다른 프로세스가 이 포트를 쓰고 있다: 'lsof -i :<port>' 로 확인)";
    case EACCES: return " (1024 미만 포트는 관리자 권한이 필요하다)";
    case EADDRNOTAVAIL: return " (이 호스트에 없는 주소다)";
    default: return "";
    }
}

int server_listen(const char *host, int port, int backlog)
{
    struct sockaddr_in sa;
    int fd = -1;
    int one = 1;
    int e;

    if (port <= 0 || port > 65535) {
        log_msg(LOG_ERROR, "잘못된 포트: %d (1~65535)", port);
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        log_msg(LOG_ERROR, "잘못된 IPv4 주소: '%s'", host);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        e = errno;
        log_msg(LOG_ERROR, "socket() 실패: %s (errno %d)", strerror(e), e);
        goto fail;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC); /* 리슨 소켓이 CGI 로 새지 않게 */
    /* 재시작 직후 이전 연결이 TIME_WAIT 이어도 같은 포트에 bind 할 수 있게 한다. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        e = errno;
        log_msg(LOG_ERROR, "setsockopt(SO_REUSEADDR) 실패: %s (errno %d)", strerror(e), e);
        goto fail;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        e = errno;
        log_msg(LOG_ERROR, "bind %s:%d 실패: %s (errno %d)%s", host, port, strerror(e), e,
                bind_hint(e));
        goto fail;
    }
    if (listen(fd, backlog) != 0) {
        e = errno;
        log_msg(LOG_ERROR, "listen() 실패: %s (errno %d)", strerror(e), e);
        goto fail;
    }
    return fd;

fail:
    if (fd >= 0)
        close(fd);
    return -1;
}

/*
 * 큐 포화: 503 을 한 번만, 블록하지 않고 보내 본 뒤 닫는다.
 * HTTPS 포트는 핸드셰이크 전이라 503 을 암호화해 보낼 수 없고, 핸드셰이크를 여기서 하면
 * accept 루프가 막힌다 → 응답 없이 닫는다 (클라이언트에게는 연결 오류로 보인다).
 */
static void reject_busy(int cfd, int tls)
{
    buf_t b;

    if (!tls && buf_init(&b, 512, 4096) == BUF_OK) {
        if (http_response_build_status_page(&b, 503, 0, 0, "Retry-After: 1\r\n") == BUF_OK)
            (void)send_nowait(cfd, b.data, b.len);
        buf_free(&b);
    }
    close(cfd);
    log_msg(LOG_WARN, "작업 큐 포화: %s 연결 종료", tls ? "HTTPS 는 응답 없이" : "503 응답 후");
}

/* 리슨 소켓 하나에서 연결 하나를 받아 큐에 넣는다. 복구 불가 오류면 -1. */
static int accept_one(const server_listener_t *l, threadpool_t *tp)
{
    threadpool_item_t item;
    int cfd = accept(l->fd, NULL, NULL);

    if (cfd < 0) {
        int e = errno;

        if (e == EINTR || e == ECONNABORTED || e == EAGAIN || e == EWOULDBLOCK)
            return 0;
        if (e == EMFILE || e == ENFILE) {
            /* fd 고갈: 잠깐 쉬어서 워커가 연결을 닫을 시간을 준다 (바쁜 루프 방지) */
            struct timespec ts = {0, 50 * 1000 * 1000};

            log_msg(LOG_WARN, "accept: fd 고갈 (%s)", strerror(e));
            nanosleep(&ts, NULL);
            return 0;
        }
        log_msg(LOG_ERROR, "accept 실패: %s (errno %d)", strerror(e), e);
        return -1;
    }
    /*
     * FD_CLOEXEC: 워커가 CGI 를 fork/exec 할 때 이 클라이언트 소켓이 CGI 프로세스로 새면,
     * CGI 가 끝날 때까지 연결이 닫히지 않는다. (accept4 는 POSIX 가 아니라 macOS 에 없어서
     * accept 직후 fcntl 로 건다. 그 사이 다른 스레드가 fork 하는 아주 짧은 창은 남는다.)
     */
    {
        /* 응답은 이미 한 번의 send 로 모아 보내므로, 작은 조각을 모으는 Nagle 은 지연만 더한다 */
        int one = 1;

        (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    if (fcntl(cfd, F_SETFD, FD_CLOEXEC) != 0 || platform_sock_init(cfd) != 0) {
        close(cfd);
        return 0;
    }
    item.fd = cfd;
    item.tls = l->tls;
    if (threadpool_try_submit(tp, item) != 0)
        reject_busy(cfd, l->tls);
    return 0;
}

int server_run(const server_listener_t *ls, size_t n, threadpool_t *tp,
               volatile sig_atomic_t *stop)
{
    struct pollfd pfd[SERVER_MAX_LISTENERS];
    size_t i;

    if (n == 0 || n > SERVER_MAX_LISTENERS)
        return -1;
    while (!*stop) {
        int pr;

        for (i = 0; i < n; i++) {
            pfd[i].fd = ls[i].fd;
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        pr = poll(pfd, (nfds_t)n, ACCEPT_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LOG_ERROR, "poll(listen) 실패: %s", strerror(errno));
            return -1;
        }
        /* 준비된 소켓마다 한 번씩 accept: 한쪽 포트가 몰려도 다른 포트가 굶지 않는다 */
        for (i = 0; i < n && pr > 0; i++) {
            if (pfd[i].revents == 0)
                continue;
            if (accept_one(&ls[i], tp) != 0)
                return -1;
        }
    }
    return 0;
}
