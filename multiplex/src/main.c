/*
 * main.c — I/O 멀티플렉싱 판 진입점: 옵션 → 시그널 → fd 한도 → listen → 이벤트 루프 → 정리.
 *
 * threadpool/ 판과 옵션 이름을 최대한 맞췄다. 다른 점:
 *   -w(워커 수), -q(큐 길이) 없음  → 스레드가 하나라 의미가 없다
 *   -c 는 "최대 동시 연결 수"        → threadpool/ 의 -c(CGI 타임아웃)와 다르다 (이 판은 CGI 없음)
 *   -f(설정 파일), -g(CGI), 프록시 없음 → 정적 파일만 제공
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* OPEN_MAX (macOS) */
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_loop.h"
#include "http_response.h"
#include "log.h"
#include "platform.h"
#include "static.h"

#define FD_HEADROOM 32 /* 연결 외에 필요한 fd: 리슨, poller, 예비 fd, 열린 파일, 표준 입출력 등 */

/*
 * 시그널 핸들러는 이 플래그만 세운다 (async-signal-safe 함수만 부를 수 있으므로).
 * 이벤트 루프는 wait 가 EINTR 로 깨거나, 최대 500ms 마다 깨어나 이 값을 확인한다.
 */
static volatile sig_atomic_t g_stop = 0;

static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-H host] [-p port] [-r root] [-c max_conns] [-t idle_timeout_ms]\n"
            "          [-T header_timeout_ms] [-m max_requests] [-n|--name 백엔드이름] [-v]\n"
            "  -H  바인드 주소 (기본 0.0.0.0)\n"
            "  -p  포트 (기본 8080)\n"
            "  -r  문서 루트 (기본 ./www)\n"
            "  -c  최대 동시 연결 수 (기본 4096, 넘으면 503. fd 한도에 맞춰 줄어들 수 있음)\n"
            "  -t  유휴 타임아웃 ms (기본 5000)\n"
            "  -T  헤더 마감 ms: 요청 첫 바이트부터 헤더 끝까지 (기본 10000, 0=끔, slowloris 방어)\n"
            "  -m  연결당 최대 요청 수 (기본 100)\n"
            "  -n  모든 응답에 'X-Backend: <이름>' (threadpool/ 리버스 프록시의 백엔드로 쓸 때)\n"
            "  -v  디버그 로그\n",
            prog);
}

static int parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static int install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    /* 끊긴 소켓에 send 해도 프로세스가 죽지 않게 (소켓 단위 방어는 platform.h) */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) != 0)
        return -1;
    /* SA_RESTART 를 주지 않는다: epoll_wait/kevent 가 EINTR 로 깨어나 플래그를 바로 확인하도록 */
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    return 0;
}

/*
 * fd 한도(soft)를 hard 까지 올리고, max_conns 를 그 안으로 맞춘다.
 * 스레드 풀은 워커 수가 동시 연결을 제한했지만, 이벤트 루프는 연결 = fd 하나라서 fd 한도가 곧 상한이다.
 * (macOS 기본 soft 한도는 256 이라 올리지 않으면 수백 연결에서 EMFILE.)
 */
static long fit_fd_limit(long want_conns)
{
    struct rlimit rl;
    rlim_t need = (rlim_t)want_conns + FD_HEADROOM;

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return want_conns;
    if (rl.rlim_cur < need) {
        rlim_t target = need;

        if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max)
            target = rl.rlim_max;
#ifdef OPEN_MAX
        if (target > OPEN_MAX) /* macOS: hard 가 unlimited 여도 OPEN_MAX 보다 크게는 못 올린다 */
            target = OPEN_MAX;
#endif
        rl.rlim_cur = target;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
            (void)getrlimit(RLIMIT_NOFILE, &rl);
    }
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < need) {
        long fit = (long)rl.rlim_cur - FD_HEADROOM;

        if (fit < 1)
            fit = 1;
        log_msg(LOG_WARN, "fd 한도 %llu: 최대 연결 수를 %ld → %ld 로 줄임 (ulimit -n 을 올리면 늘어남)",
                (unsigned long long)rl.rlim_cur, want_conns, fit);
        return fit;
    }
    return want_conns;
}

static const char *bind_hint(int e)
{
    switch (e) {
    case EADDRINUSE: return " (이미 다른 프로세스가 이 포트를 쓰고 있다: 'lsof -i :<port>' 로 확인)";
    case EACCES: return " (1024 미만 포트는 관리자 권한이 필요하다)";
    case EADDRNOTAVAIL: return " (이 호스트에 없는 주소다)";
    default: return "";
    }
}

/* IPv4 리슨 소켓. 실패 시 원인을 로그로 남기고 -1. */
static int listen_on(const char *host, int port, int backlog)
{
    struct sockaddr_in sa;
    int fd;
    int one = 1;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        log_msg(LOG_ERROR, "잘못된 IPv4 주소: '%s'", host);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg(LOG_ERROR, "socket() 실패: %s", strerror(errno));
        return -1;
    }
    /* 재시작 직후 이전 연결이 TIME_WAIT 이어도 같은 포트에 bind 할 수 있게 한다. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        log_msg(LOG_ERROR, "setsockopt(SO_REUSEADDR) 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;

        log_msg(LOG_ERROR, "bind %s:%d 실패: %s (errno %d)%s", host, port, strerror(e), e,
                bind_hint(e));
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) != 0) {
        log_msg(LOG_ERROR, "listen() 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* "--name X" / "--name=X" 를 argv 에서 꺼낸다 (getopt_long 은 POSIX 가 아니다). threadpool/ 과 같다. */
static int extract_long_name(int argc, char **argv, char **out, const char **name)
{
    int i, n = 0;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0) {
            if (i + 1 >= argc)
                return -1;
            *name = argv[++i];
        } else if (strncmp(argv[i], "--name=", 7) == 0) {
            *name = argv[i] + 7;
        } else {
            out[n++] = argv[i];
        }
    }
    out[n] = NULL;
    return n;
}

int main(int argc, char **argv)
{
    static static_ctx_t sctx; /* PATH_MAX 배열: 스택 대신 정적 저장소 */
    static char *args[256];
    const http_limits_t default_limits = HTTP_LIMITS_DEFAULT;
    const char *host = "0.0.0.0";
    const char *root = "./www";
    const char *name = NULL;
    long port = 8080, max_conns = 4096, idle = 5000, hdr = 10000, maxreq = 100;
    int verbose = 0;
    int nargs, opt;
    int lfd = -1;
    int rc = 1;
    ev_loop_t *loop = NULL;
    ev_config_t ecfg;
    ev_stats_t st;

    if (argc >= (int)(sizeof args / sizeof args[0])) {
        fprintf(stderr, "인자가 너무 많다\n");
        return 2;
    }
    nargs = extract_long_name(argc, argv, args, &name);
    if (nargs < 0) {
        fprintf(stderr, "--name 에 값이 필요하다\n");
        return 2;
    }
    while ((opt = getopt(nargs, args, "H:p:r:c:t:T:m:n:vh")) != -1) {
        int bad = 0;

        switch (opt) {
        case 'H': host = optarg; break;
        case 'r': root = optarg; break;
        case 'n': name = optarg; break;
        case 'p': bad = parse_long(optarg, 1, 65535, &port); break;
        case 'c': bad = parse_long(optarg, 1, 1000000, &max_conns); break;
        case 't': bad = parse_long(optarg, 1, 3600 * 1000, &idle); break;
        case 'T': bad = parse_long(optarg, 0, 3600 * 1000, &hdr); break;
        case 'm': bad = parse_long(optarg, 1, 1000000, &maxreq); break;
        case 'v': verbose = 1; break;
        default: usage(args[0]); return opt == 'h' ? 0 : 2;
        }
        if (bad) {
            fprintf(stderr, "잘못된 값: -%c %s\n", opt, optarg);
            usage(args[0]);
            return 2;
        }
    }

    log_init(verbose ? LOG_DEBUG : LOG_INFO, NULL);
    if (name != NULL)
        http_response_set_backend_name(name);
    if (install_signals() != 0) {
        log_msg(LOG_ERROR, "시그널 설정 실패: %s", strerror(errno));
        return 1;
    }
    if (static_init(&sctx, root) != 0) {
        log_msg(LOG_ERROR, "문서 루트를 열 수 없음: '%s' (%s)", root, strerror(errno));
        return 1;
    }
    max_conns = fit_fd_limit(max_conns);

    /* backlog 는 크게: 연결이 몰릴 때 루프가 accept 하기 전까지 커널이 붙잡아 둘 수 있는 수 */
    lfd = listen_on(host, (int)port, 1024);
    if (lfd < 0)
        goto cleanup;

    memset(&ecfg, 0, sizeof ecfg);
    ecfg.limits = default_limits;
    ecfg.idle_timeout_ms = (int)idle;
    ecfg.header_timeout_ms = (int)hdr;
    ecfg.send_timeout_ms = 10000;
    ecfg.max_requests = (int)maxreq;
    ecfg.max_conns = (int)max_conns;
    ecfg.shutdown_grace_ms = 5000;
    ecfg.sctx = &sctx;
    ecfg.stop = &g_stop;
    loop = ev_loop_create(&ecfg, lfd);
    if (loop == NULL) {
        log_msg(LOG_ERROR, "이벤트 루프 생성 실패: %s", strerror(errno));
        goto cleanup;
    }

    log_msg(LOG_INFO, "listening on http://%s:%ld  root=%s  model=event-loop(%s, 1 thread)  "
                      "max_conns=%ld idle=%ldms%s%s",
            host, port, sctx.root, PLATFORM_POLLER_NAME, max_conns, idle,
            name ? "  backend-name=" : "", name ? name : "");
    rc = ev_loop_run(loop) == 0 ? 0 : 1;

cleanup:
    /*
     * 종료 순서 (스레드가 하나라 threadpool/ 보다 단순하다: join 할 워커가 없다)
     * 1. ev_loop_run 안: accept 중단, 대기 연결 닫기, 진행 중 응답 마무리 (최대 5초)
     * 2. 리슨 소켓 close
     * 3. 루프 해제: 남은 연결(시간 초과분) 강제 close, 타이머·poller 해제
     */
    if (lfd >= 0) {
        close(lfd);
        log_msg(LOG_INFO, "종료 2/3: 리슨 소켓 닫음");
    }
    if (loop != NULL) {
        ev_loop_stats(loop, &st);
        log_msg(LOG_INFO, "stats: accepted=%llu requests=%llu busy503=%llu emfile=%llu timeouts=%llu "
                          "peak_conns=%zu",
                st.accepted, st.requests, st.rejected_busy, st.rejected_emfile, st.timeouts,
                st.peak_conns);
        ev_loop_destroy(loop);
    }
    log_msg(LOG_INFO, "종료 3/3: %s", rc == 0 ? "정상 종료" : "오류로 종료");
    return rc;
}
