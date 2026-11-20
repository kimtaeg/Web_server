/*
 * server.c — listen 소켓과 accept 루프.
 */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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

/* 큐 포화: 503 을 한 번만, 블록하지 않고 보내 본 뒤 닫는다. */
static void reject_busy(int cfd)
{
    buf_t b;

    if (buf_init(&b, 512, 4096) == BUF_OK &&
        http_response_build_status_page(&b, 503, 0, 0, "Retry-After: 1\r\n") == BUF_OK)
        (void)send_nowait(cfd, b.data, b.len);
    buf_free(&b);
    close(cfd);
    log_msg(LOG_WARN, "작업 큐 포화: 503 응답 후 연결 종료");
}

int server_run(int lfd, threadpool_t *tp, volatile sig_atomic_t *stop)
{
    while (!*stop) {
        struct pollfd pfd;
        int pr;
        int cfd;

        pfd.fd = lfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, ACCEPT_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LOG_ERROR, "poll(listen) 실패: %s", strerror(errno));
            return -1;
        }
        if (pr == 0)
            continue;

        cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            int e = errno;

            if (e == EINTR || e == ECONNABORTED || e == EAGAIN || e == EWOULDBLOCK)
                continue;
            if (e == EMFILE || e == ENFILE) {
                /* fd 고갈: 잠깐 쉬어서 워커가 연결을 닫을 시간을 준다 (바쁜 루프 방지) */
                struct timespec ts = {0, 50 * 1000 * 1000};

                log_msg(LOG_WARN, "accept: fd 고갈 (%s)", strerror(e));
                nanosleep(&ts, NULL);
                continue;
            }
            log_msg(LOG_ERROR, "accept 실패: %s (errno %d)", strerror(e), e);
            return -1;
        }
        if (platform_sock_init(cfd) != 0) {
            close(cfd);
            continue;
        }
        if (threadpool_try_submit(tp, cfd) != 0)
            reject_busy(cfd);
    }
    return 0;
}
