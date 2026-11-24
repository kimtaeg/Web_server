/*
 * test_platform.c — platform.h 의 SIGPIPE 방어가 실제로 동작하는지 확인.
 *
 * SIGPIPE 를 일부러 기본 동작(프로세스 종료)으로 두고, 상대가 닫힌 소켓에 send 한다.
 * platform_sock_init + PLATFORM_SEND_FLAGS 가 제대로라면 프로세스가 죽지 않고 EPIPE 를 받는다.
 * 잘못됐다면 이 테스트 프로세스가 시그널로 죽고 러너가 실패로 표시한다.
 *
 * + poller(epoll/kqueue) 래퍼: 두 OS 에서 같은 의미(level-triggered, 관심 켜기/끄기, EOF=readable)인지.
 */
#include "test.h"

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

static void test_send_to_closed_peer_returns_epipe(void)
{
    int sv[2];
    ssize_t n;

    REQUIRE(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK_EQ_INT(platform_sock_init(sv[0]), 0);
    close(sv[1]);

    n = send(sv[0], "x", 1, PLATFORM_SEND_FLAGS);
    CHECK_EQ_INT(n, -1);
    CHECK_EQ_INT(errno, EPIPE);
    close(sv[0]);
}

static long long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* fd 에 대한 이벤트만 모아서: 1=readable, 2=writable 비트 */
static int events_for(int pfd, int fd, int timeout_ms)
{
    plat_event_t evs[8];
    int n = plat_poller_wait(pfd, evs, 8, timeout_ms);
    int i, bits = 0;

    for (i = 0; i < n; i++) {
        if (evs[i].fd != fd)
            continue;
        bits |= (evs[i].readable ? 1 : 0) | (evs[i].writable ? 2 : 0);
    }
    return bits;
}

static void test_poller_semantics(void)
{
    int pfd = plat_poller_create();
    int sv[2];
    char c;
    long long t0;

    REQUIRE(pfd >= 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* 읽기 관심만: 데이터가 없으면 타임아웃까지 아무것도 없다 */
    CHECK_EQ_INT(plat_poller_add(pfd, sv[0], 1, 0), 0);
    t0 = ms_now();
    CHECK_EQ_INT(events_for(pfd, sv[0], 50), 0);
    CHECK(ms_now() - t0 >= 40); /* 타임아웃을 실제로 기다렸다 */

    /* 데이터가 오면 readable. level-triggered: 읽지 않으면 다음 wait 에서도 또 알려 준다 */
    CHECK_EQ_INT((int)write(sv[1], "x", 1), 1);
    CHECK_EQ_INT(events_for(pfd, sv[0], 100), 1);
    CHECK_EQ_INT(events_for(pfd, sv[0], 100), 1);
    CHECK_EQ_INT((int)read(sv[0], &c, 1), 1);
    CHECK_EQ_INT(events_for(pfd, sv[0], 30), 0); /* 다 읽으면 조용하다 */

    /* 쓰기 관심을 켜면 (송신 버퍼가 비어 있으므로) 곧바로 writable, 끄면 다시 조용하다 */
    CHECK_EQ_INT(plat_poller_mod(pfd, sv[0], 1, 1), 0);
    CHECK_EQ_INT(events_for(pfd, sv[0], 100), 2);
    CHECK_EQ_INT(plat_poller_mod(pfd, sv[0], 1, 0), 0);
    CHECK_EQ_INT(events_for(pfd, sv[0], 30), 0);

    /* 관심을 모두 끄면 데이터가 와도 알리지 않는다 (응답 중 읽기를 멈추는 데 쓴다) */
    CHECK_EQ_INT(plat_poller_mod(pfd, sv[0], 0, 0), 0);
    CHECK_EQ_INT((int)write(sv[1], "y", 1), 1);
    CHECK_EQ_INT(events_for(pfd, sv[0], 30), 0);
    CHECK_EQ_INT(plat_poller_mod(pfd, sv[0], 1, 0), 0);
    CHECK_EQ_INT(events_for(pfd, sv[0], 100), 1);
    CHECK_EQ_INT((int)read(sv[0], &c, 1), 1);

    /* 상대 종료(EOF)는 readable 로 온다: recv 가 0 을 돌려 확인 */
    close(sv[1]);
    CHECK_EQ_INT(events_for(pfd, sv[0], 100) & 1, 1);
    CHECK_EQ_INT((int)read(sv[0], &c, 1), 0);

    /* 삭제 후에는 이벤트가 없다. 두 번 삭제하면 오류지만 죽지 않는다 */
    CHECK_EQ_INT(plat_poller_del(pfd, sv[0]), 0);
    CHECK_EQ_INT(events_for(pfd, sv[0], 30), 0);
    close(sv[0]);
    close(pfd);
}

int main(void)
{
    RUN_TEST(test_send_to_closed_peer_returns_epipe);
    RUN_TEST(test_poller_semantics);
    TEST_END();
}
