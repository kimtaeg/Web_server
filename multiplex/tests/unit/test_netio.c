/*
 * test_netio.c — send_all / recv_some / send_nowait (socketpair 사용).
 */
#include "test.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "netio.h"
#include "platform.h"

#define BIG (4 * 1024 * 1024)

static long ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

typedef struct {
    int fd;
    size_t total;
    unsigned long sum;
} reader_t;

static void *reader_main(void *p)
{
    reader_t *r = p;
    char buf[8192];
    ssize_t n;

    while ((n = recv_some(r->fd, buf, sizeof buf, 5000)) > 0) {
        ssize_t i;

        for (i = 0; i < n; i++)
            r->sum += (unsigned char)buf[i];
        r->total += (size_t)n;
    }
    return NULL;
}

/* 소켓 버퍼(수 KB)보다 훨씬 큰 데이터: send 가 여러 번 부분 전송되어도 전부 도착해야 한다. */
static void test_send_all_large(void)
{
    int sv[2];
    char *data = malloc(BIG);
    unsigned long sum = 0;
    size_t i;
    pthread_t th;
    reader_t r;

    REQUIRE(data != NULL);
    for (i = 0; i < BIG; i++) {
        data[i] = (char)(i * 31 + 7);
        sum += (unsigned char)data[i];
    }
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    platform_sock_init(sv[0]);
    memset(&r, 0, sizeof r);
    r.fd = sv[1];
    pthread_create(&th, NULL, reader_main, &r);

    CHECK_EQ_INT(send_all(sv[0], data, BIG), BIG);
    close(sv[0]); /* EOF → 리더 종료 */
    pthread_join(th, NULL);
    CHECK_EQ_SIZE(r.total, BIG);
    CHECK(r.sum == sum);
    close(sv[1]);
    free(data);
}

static void test_recv_timeout_and_eof(void)
{
    int sv[2];
    char buf[16];
    long t0;
    ssize_t n;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    t0 = ms_now();
    n = recv_some(sv[1], buf, sizeof buf, 100);
    CHECK_EQ_INT(n, NETIO_TIMEOUT);
    CHECK(ms_now() - t0 >= 90);

    CHECK_EQ_INT(send_all(sv[0], "hi", 2), 2);
    n = recv_some(sv[1], buf, sizeof buf, 1000);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_MEM(buf, "hi", 2);

    close(sv[0]);
    CHECK_EQ_INT(recv_some(sv[1], buf, sizeof buf, 1000), 0); /* EOF */
    close(sv[1]);
}

static void test_send_to_closed_peer(void)
{
    int sv[2];

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    platform_sock_init(sv[0]);
    close(sv[1]);
    CHECK_EQ_INT(send_all(sv[0], "x", 1), -1); /* 시그널로 죽지 않고 -1 */
    close(sv[0]);
}

/* 상대가 읽지 않아 버퍼가 가득 차도 send_nowait 은 블록하지 않는다. */
static void test_send_nowait_never_blocks(void)
{
    int sv[2];
    char chunk[4096];
    int i;
    ssize_t last = 0;
    long t0;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    platform_sock_init(sv[0]);
    memset(chunk, 'z', sizeof chunk);
    t0 = ms_now();
    for (i = 0; i < 1000 && last >= 0; i++)
        last = send_nowait(sv[0], chunk, sizeof chunk);
    CHECK_EQ_INT(last, -1); /* 결국 EAGAIN */
    CHECK(ms_now() - t0 < 1000);
    close(sv[0]);
    close(sv[1]);
}

/* SO_SNDTIMEO: 상대가 안 읽으면 send_all 이 영원히 막히지 않고 -1. */
static void test_send_timeout(void)
{
    int sv[2];
    char *data = malloc(BIG);
    long t0;

    REQUIRE(data != NULL);
    memset(data, 'y', BIG);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    platform_sock_init(sv[0]);
    CHECK_EQ_INT(netio_set_send_timeout(sv[0], 200), 0);
    t0 = ms_now();
    CHECK_EQ_INT(send_all(sv[0], data, BIG), -1);
    CHECK(ms_now() - t0 < 3000);
    close(sv[0]);
    close(sv[1]);
    free(data);
}

/* 논블로킹 헬퍼: 데이터 없음 → AGAIN, 데이터 → 바이트 수, 상대 종료 → 0 / 버퍼 가득 → AGAIN */
static void test_nonblocking_helpers(void)
{
    int sv[2];
    char buf[64];
    char big[8192];
    ssize_t n;
    int fl, rounds = 0;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    platform_sock_init(sv[0]);
    fl = fcntl(sv[0], F_GETFL);
    REQUIRE(fcntl(sv[0], F_SETFL, fl | O_NONBLOCK) == 0);
    fl = fcntl(sv[1], F_GETFL);
    REQUIRE(fcntl(sv[1], F_SETFL, fl | O_NONBLOCK) == 0);

    CHECK_EQ_INT((int)netio_recv_nb(sv[0], buf, sizeof buf), NETIO_AGAIN);
    CHECK_EQ_INT((int)netio_send_nb(sv[1], "abc", 3), 3);
    CHECK_EQ_INT((int)netio_recv_nb(sv[0], buf, sizeof buf), 3);
    CHECK_EQ_MEM(buf, "abc", 3);

    /* 상대가 읽지 않으면 언젠가 송신 버퍼가 가득 차 AGAIN (블록하지 않는다) */
    memset(big, 'z', sizeof big);
    while ((n = netio_send_nb(sv[0], big, sizeof big)) > 0 && rounds < 100000)
        rounds++;
    CHECK_EQ_INT((int)n, NETIO_AGAIN);

    close(sv[1]);
    /* 상대 종료 후: 받을 쪽은 EOF(0), 보낼 쪽은 오류(-1, SIGPIPE 없이) */
    CHECK_EQ_INT((int)netio_send_nb(sv[0], "x", 1), -1);
    close(sv[0]);

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    CHECK_EQ_INT((int)netio_recv_nb(sv[0], buf, sizeof buf), 0);
    close(sv[0]);
}

int main(void)
{
    RUN_TEST(test_send_all_large);
    RUN_TEST(test_recv_timeout_and_eof);
    RUN_TEST(test_send_to_closed_peer);
    RUN_TEST(test_send_nowait_never_blocks);
    RUN_TEST(test_send_timeout);
    RUN_TEST(test_nonblocking_helpers);
    TEST_END();
}
