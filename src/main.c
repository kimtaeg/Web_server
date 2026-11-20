/*
 * main.c — 진입점: 옵션 파싱 → 시그널 설정 → listen → 워커 풀 → accept 루프 → 정리.
 *
 * 설정 파일(config 모듈)은 3단계에서 들어온다. 지금은 명령줄 옵션만 받는다.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgi.h"
#include "conn.h"
#include "log.h"
#include "router.h"
#include "server.h"
#include "static.h"
#include "threadpool.h"

/*
 * 시그널 핸들러는 이 플래그만 세운다. 핸들러 안에서는 async-signal-safe 함수만 부를 수 있어서
 * 로그 출력(내부 락), malloc, 스레드 정리 같은 일은 할 수 없다. 실제 정리는 메인 흐름이 한다.
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
            "사용법: %s [-H host] [-p port] [-r root] [-w workers] [-q queue]\n"
            "          [-t idle_timeout_ms] [-m max_requests] [-g cgi_dir] [-c cgi_timeout_ms] [-v]\n"
            "  -H  바인드 주소 (기본 0.0.0.0)\n"
            "  -p  포트 (기본 8080)\n"
            "  -r  문서 루트 (기본 ./www)\n"
            "  -w  워커 스레드 수 (기본 8)\n"
            "  -q  대기 큐 길이 (기본 64, 가득 차면 503)\n"
            "  -t  유휴 타임아웃 ms (기본 5000)\n"
            "  -m  연결당 최대 요청 수 (기본 100)\n"
            "  -g  CGI 디렉터리: /cgi-bin/<이름> 을 여기서 실행 (없으면 CGI 비활성)\n"
            "  -c  CGI 타임아웃 ms (기본 5000, 초과 시 504)\n"
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
    /* SA_RESTART 를 주지 않는다: 블록 중인 시스템 콜이 EINTR 로 깨어나 플래그를 확인하도록 */
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    /* PATH_MAX 배열을 담으므로 스택 대신 정적 저장소. 시작 시 한 번 쓰고 이후 읽기 전용 */
    static static_ctx_t sctx;
    static cgi_ctx_t cctx;
    static router_t router;
    const char *cgi_dir = NULL;
    long cgi_ms = CGI_DEFAULT_TIMEOUT_MS;
    const http_limits_t default_limits = HTTP_LIMITS_DEFAULT;
    const char *host = "0.0.0.0";
    const char *root = "./www";
    long port = 8080, workers = 8, queue = 64, idle_ms = 5000, max_req = 100;
    int verbose = 0;
    int opt;
    int lfd = -1;
    int rc = 1;
    threadpool_t *tp = NULL;
    conn_config_t ccfg;

    while ((opt = getopt(argc, argv, "H:p:r:w:q:t:m:g:c:vh")) != -1) {
        int bad = 0;

        switch (opt) {
        case 'H': host = optarg; break;
        case 'r': root = optarg; break;
        case 'p': bad = parse_long(optarg, 1, 65535, &port); break;
        case 'w': bad = parse_long(optarg, 1, 1024, &workers); break;
        case 'q': bad = parse_long(optarg, 1, 65536, &queue); break;
        case 't': bad = parse_long(optarg, 1, 3600 * 1000, &idle_ms); break;
        case 'm': bad = parse_long(optarg, 1, 1000000, &max_req); break;
        case 'g': cgi_dir = optarg; break;
        case 'c': bad = parse_long(optarg, 1, 3600 * 1000, &cgi_ms); break;
        case 'v': verbose = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
        if (bad) {
            fprintf(stderr, "잘못된 값: -%c %s\n", opt, optarg);
            usage(argv[0]);
            return 2;
        }
    }

    log_init(verbose ? LOG_DEBUG : LOG_INFO, NULL);
    if (install_signals() != 0) {
        log_msg(LOG_ERROR, "시그널 설정 실패: %s", strerror(errno));
        return 1;
    }
    if (static_init(&sctx, root) != 0) {
        log_msg(LOG_ERROR, "문서 루트를 열 수 없음: '%s' (%s)", root, strerror(errno));
        return 1;
    }
    /* 라우팅: /cgi-bin/ → CGI, 나머지 → 정적 파일 */
    router_init(&router, static_handle, &sctx);
    if (cgi_dir != NULL) {
        if (cgi_init(&cctx, cgi_dir, "/cgi-bin", (int)cgi_ms) != 0) {
            log_msg(LOG_ERROR, "CGI 디렉터리를 열 수 없음: '%s' (%s)", cgi_dir, strerror(errno));
            return 1;
        }
        if (cctx.dir_len > sctx.root_len && strncmp(cctx.dir, sctx.root, sctx.root_len) == 0 &&
            cctx.dir[sctx.root_len] == '/')
            log_msg(LOG_WARN, "CGI 디렉터리가 문서 루트 안에 있다: 실행 파일이 정적 파일로 노출될 수 있음");
        router_add(&router, "/cgi-bin", cgi_handle, &cctx);
    }

    lfd = server_listen(host, (int)port, 128);
    if (lfd < 0)
        goto cleanup; /* 원인은 server_listen 이 이미 로그로 남겼다 */

    memset(&ccfg, 0, sizeof ccfg);
    ccfg.limits = default_limits;
    ccfg.idle_timeout_ms = (int)idle_ms;
    ccfg.send_timeout_ms = 10000;
    ccfg.max_requests = (int)max_req;
    ccfg.handler = router_handle;
    ccfg.handler_ctx = &router;
    ccfg.stop = &g_stop;

    tp = threadpool_create((size_t)workers, (size_t)queue, conn_worker, &ccfg);
    if (tp == NULL) {
        log_msg(LOG_ERROR, "워커 풀 생성 실패");
        goto cleanup;
    }

    log_msg(LOG_INFO, "listening on http://%s:%ld  root=%s  workers=%ld queue=%ld idle=%ldms", host,
            port, sctx.root, workers, queue, idle_ms);
    if (cgi_dir != NULL)
        log_msg(LOG_INFO, "CGI: /cgi-bin/ -> %s (timeout %ldms)", cctx.dir, cgi_ms);
    rc = server_run(lfd, tp, &g_stop) == 0 ? 0 : 1;
    log_msg(LOG_INFO, "종료 시작 (signal)");

cleanup:
    /*
     * graceful shutdown 순서 — 각 단계가 앞 단계에 의존한다:
     * 1. 리슨 소켓 close  : 새 연결을 더 받지 않는다. (커널 backlog 에서 아직 accept 안 된
     *                       연결은 클라이언트 쪽에서 RST 를 받는다)
     * 2. 큐 close          : submit 을 막고 잠든 워커를 전부 깨운다.
     * 3. 진행 중 요청 완료 : 워커는 처리 중인 요청과 큐에 남은 연결을 끝낸다.
     *                       keep-alive 대기 중인 연결은 conn 이 250ms 안에 종료 플래그를 보고 닫는다.
     * 4. join              : 모든 워커가 끝날 때까지 기다린다. 이후로는 공유 자원을 쓰는 스레드가 없다.
     * 5. 자원 해제         : 그래서 이 시점에만 풀·큐·동기화 객체를 안전하게 free 할 수 있다.
     */
    if (lfd >= 0) {
        close(lfd);
        log_msg(LOG_INFO, "종료 1/4: 리슨 소켓 닫음");
    }
    if (tp != NULL) {
        threadpool_close(tp);
        log_msg(LOG_INFO, "종료 2/4: 작업 큐 닫음, 워커 깨움 (진행 중 요청 마무리 대기)");
        threadpool_join_free(tp);
        tp = NULL;
        log_msg(LOG_INFO, "종료 3/4: 워커 전부 join, 풀 해제");
    }
    log_msg(LOG_INFO, "종료 4/4: %s", rc == 0 ? "정상 종료" : "오류로 종료");
    return rc;
}
