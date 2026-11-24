/*
 * platform.h — Linux / macOS 차이를 모아 두는 유일한 곳.
 *
 * 1. SIGPIPE 방어: Linux 는 send 플래그(MSG_NOSIGNAL), macOS 는 소켓 옵션(SO_NOSIGPIPE).
 * 2. I/O 멀티플렉싱: Linux 는 epoll, macOS 는 kqueue. 이벤트 루프는 아래 plat_poller_* 만 쓴다.
 *
 * [poller 의미 — 두 OS 에서 같게 맞춘 것]
 * - level-triggered: 읽을 데이터가 남아 있으면 다음 wait 에서도 계속 알려 준다.
 *   (edge-triggered 는 "새로 생겼을 때 한 번"만 알려서, EAGAIN 까지 다 읽지 않으면 그 연결이
 *    영원히 깨어나지 않는다. 한 번의 실수가 멈춤으로 이어지므로 정확성을 우선해 LT 를 쓴다.)
 * - want_read / want_write 로 관심을 표현한다. 보낼 게 없는데 write 관심을 켜 두면
 *   소켓 버퍼가 비어 있는 한 매번 깨어나 busy loop 가 되므로, 보낼 게 있을 때만 켠다.
 * - kqueue 는 읽기/쓰기가 별도 필터라 한 fd 가 이벤트 2개로 올 수 있다. 이벤트 루프는 같은 fd 가
 *   두 번 와도 안전하게 처리한다 (상태 머신이 현재 상태 기준으로만 움직이므로).
 * - 오류/끊김(EPOLLERR|EPOLLHUP, EV_EOF)은 error 가 아니라 readable 로 알린다. 그래야 recv 가
 *   0 이나 오류를 직접 돌려줘서, "상대가 쓰기만 닫고 응답은 기다리는" half-close 도 구분할 수 있다.
 *
 * 성능 특성: 둘 다 "준비된 fd 수"에 비례한다 (poll/select 는 감시 fd 수 전체를 매번 훑는다).
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    int readable; /* 읽기 가능, 또는 상대 종료/오류 (recv 로 확인) */
    int writable;
} plat_event_t;

#if defined(__linux__)

#include <sys/epoll.h>

#define PLATFORM_SEND_FLAGS MSG_NOSIGNAL
#define PLATFORM_POLLER_NAME "epoll"

/* Linux 는 send 플래그로 막으므로 소켓 단위 설정이 필요 없다. 0=성공 */
static inline int platform_sock_init(int fd)
{
    (void)fd;
    return 0;
}

/* 반환: poller fd, 실패 -1. CLOEXEC 로 만든다. */
static inline int plat_poller_create(void)
{
    return epoll_create1(EPOLL_CLOEXEC);
}

static inline int plat__epoll_ctl(int pfd, int op, int fd, int want_read, int want_write)
{
    struct epoll_event ev;

    ev.events = (want_read ? EPOLLIN : 0u) | (want_write ? EPOLLOUT : 0u);
    ev.data.fd = fd;
    return epoll_ctl(pfd, op, fd, &ev);
}

static inline int plat_poller_add(int pfd, int fd, int want_read, int want_write)
{
    return plat__epoll_ctl(pfd, EPOLL_CTL_ADD, fd, want_read, want_write);
}

static inline int plat_poller_mod(int pfd, int fd, int want_read, int want_write)
{
    return plat__epoll_ctl(pfd, EPOLL_CTL_MOD, fd, want_read, want_write);
}

/* close 전에 부른다 (close 가 자동으로 빼 주지만, dup 된 fd 가 있으면 남는다). */
static inline int plat_poller_del(int pfd, int fd)
{
    struct epoll_event ev = {0}; /* 2.6.9 이전 커널은 NULL 을 거부했다 */

    return epoll_ctl(pfd, EPOLL_CTL_DEL, fd, &ev);
}

/* 최대 max 개. 반환: 이벤트 수(EINTR 이면 0), 오류 -1. timeout_ms < 0 이면 무한 대기. */
static inline int plat_poller_wait(int pfd, plat_event_t *out, int max, int timeout_ms)
{
    struct epoll_event evs[256];
    int n, i;

    if (max > 256)
        max = 256;
    n = epoll_wait(pfd, evs, max, timeout_ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        out[i].fd = evs[i].data.fd;
        out[i].readable = (evs[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
        out[i].writable = (evs[i].events & EPOLLOUT) != 0;
    }
    return n;
}

#elif defined(__APPLE__)

#include <stdint.h> /* uintptr_t */
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

/* SO_NOSIGPIPE 는 _DARWIN_C_SOURCE 에서만 선언된다 (Makefile 이 정의). */
#ifndef SO_NOSIGPIPE
#error "SO_NOSIGPIPE 가 보이지 않음: -D_DARWIN_C_SOURCE 로 빌드해야 한다"
#endif

#define PLATFORM_SEND_FLAGS 0
#define PLATFORM_POLLER_NAME "kqueue"

/* accept/socket 직후 호출. 0=성공, -1=실패(errno 설정) */
static inline int platform_sock_init(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

#include <fcntl.h>

/* kqueue 는 생성 플래그로 CLOEXEC 를 줄 수 없어 fcntl 로 붙인다 (이 구현은 fork 하지 않지만 일관성). */
static inline int plat_poller_create(void)
{
    int kq = kqueue();

    if (kq >= 0 && fcntl(kq, F_SETFD, FD_CLOEXEC) != 0) {
        close(kq);
        return -1;
    }
    return kq;
}

/*
 * epoll 의 "관심 집합 통째로 교체(MOD)"를 kqueue 의 필터 두 개(READ, WRITE) 켜기/끄기로 흉내 낸다.
 * 등록 안 된 필터를 EV_DELETE 하면 ENOENT 가 나므로 그 오류는 무시한다 (kevent 의 receipt 모드로
 * 필터별 결과를 받아 ENOENT 만 걸러 낸다).
 */
static inline int plat__kq_set(int kq, int fd, int want_read, int want_write)
{
    struct kevent ch[2];
    struct kevent res[2];
    int n, i;

    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, (want_read ? EV_ADD | EV_ENABLE : EV_DELETE) | EV_RECEIPT,
           0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE,
           (want_write ? EV_ADD | EV_ENABLE : EV_DELETE) | EV_RECEIPT, 0, 0, NULL);
    n = kevent(kq, ch, 2, res, 2, NULL);
    if (n < 0)
        return -1;
    for (i = 0; i < n; i++) {
        if ((res[i].flags & EV_ERROR) && res[i].data != 0 && res[i].data != ENOENT) {
            errno = (int)res[i].data;
            return -1;
        }
    }
    return 0;
}

static inline int plat_poller_add(int pfd, int fd, int want_read, int want_write)
{
    return plat__kq_set(pfd, fd, want_read, want_write);
}

static inline int plat_poller_mod(int pfd, int fd, int want_read, int want_write)
{
    return plat__kq_set(pfd, fd, want_read, want_write);
}

static inline int plat_poller_del(int pfd, int fd)
{
    return plat__kq_set(pfd, fd, 0, 0);
}

static inline int plat_poller_wait(int pfd, plat_event_t *out, int max, int timeout_ms)
{
    struct kevent evs[256];
    struct timespec ts;
    int n, i;

    if (max > 256)
        max = 256;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    }
    n = kevent(pfd, NULL, 0, evs, max, timeout_ms >= 0 ? &ts : NULL);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        out[i].fd = (int)evs[i].ident;
        /* EV_EOF/EV_ERROR 도 readable 로: recv 가 0 또는 오류로 실제 상태를 알려 준다 */
        out[i].readable = evs[i].filter == EVFILT_READ || (evs[i].flags & (EV_EOF | EV_ERROR)) != 0;
        out[i].writable = evs[i].filter == EVFILT_WRITE;
    }
    return n;
}

#else
#error "지원하지 않는 플랫폼 (Linux, macOS 만 지원)"
#endif

/* 정적 파일은 고정 버퍼 read/send 로 보내므로 sendfile 분기는 두지 않는다 (DESIGN.md 16절). */

#endif /* PLATFORM_H */
