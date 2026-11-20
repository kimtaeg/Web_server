/*
 * test_platform.c — platform.h 의 SIGPIPE 방어가 실제로 동작하는지 확인.
 *
 * SIGPIPE 를 일부러 기본 동작(프로세스 종료)으로 두고, 상대가 닫힌 소켓에 send 한다.
 * platform_sock_init + PLATFORM_SEND_FLAGS 가 제대로라면 프로세스가 죽지 않고 EPIPE 를 받는다.
 * 잘못됐다면 이 테스트 프로세스가 시그널로 죽고 러너가 실패로 표시한다.
 */
#include "test.h"

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
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

int main(void)
{
    RUN_TEST(test_send_to_closed_peer_returns_epipe);
    TEST_END();
}
