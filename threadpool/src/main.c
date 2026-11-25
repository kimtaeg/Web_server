/*
 * main.c — 진입점: 옵션/설정 → 시그널 → 라우팅 구성 → listen → 워커 풀 → accept 루프 → 정리.
 *
 * 설정 우선순위: 기본값 < 설정 파일(-f) < 명령줄 옵션.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgi.h"
#include "config.h"
#include "conn.h"
#include "fproxy.h"
#include "http_response.h"
#include "log.h"
#include "router.h"
#include "rproxy.h"
#include "server.h"
#include "static.h"
#include "threadpool.h"
#include "tls.h"
#include "upstream.h"

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
            "사용법: %s [-f config] [-H host] [-p port] [-r root] [-w workers] [-q queue]\n"
            "          [-t idle_timeout_ms] [-m max_requests] [-g cgi_dir] [-c cgi_timeout_ms]\n"
            "          [-n|--name 백엔드이름] [-C cert.pem -K key.pem [-s https_port]] [-v]\n"
            "  -f  설정 파일 (route 등). 명령줄 옵션이 설정 파일보다 우선\n"
            "  -H  바인드 주소 (기본 0.0.0.0)\n"
            "  -p  포트 (기본 8080)\n"
            "  -r  문서 루트 (기본 ./www)\n"
            "  -w  워커 스레드 수 (기본 8)\n"
            "  -q  대기 큐 길이 (기본 64, 가득 차면 503)\n"
            "  -t  유휴 타임아웃 ms (기본 5000)\n"
            "  -T  헤더 마감 ms: 요청 첫 바이트부터 헤더 끝까지 (기본 10000, slowloris 방어)\n"
            "  -m  연결당 최대 요청 수 (기본 100)\n"
            "  -g  CGI 디렉터리: /cgi-bin/<이름> 을 여기서 실행 (없으면 CGI 비활성)\n"
            "  -c  CGI 타임아웃 ms (기본 5000, 초과 시 504)\n"
            "  -n  백엔드 모드: 모든 응답에 'X-Backend: <이름>' (로드밸런싱 확인용)\n"
            "  -C  TLS 인증서(PEM). -K 와 함께 주면 HTTPS 를 같은 주소의 -s 포트에서 추가로 연다\n"
            "  -K  TLS 개인키(PEM)\n"
            "  -s  HTTPS 포트 (기본 8443)\n"
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

/*
 * "--name X" / "--name=X" 를 argv 에서 꺼내고 나머지를 out 으로 옮긴다.
 * getopt_long 은 POSIX 가 아니라서(GNU/BSD 확장) 쓰지 않는다. 짧은 형식 "-n X" 는 getopt 가 처리.
 */
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

/* 명령줄에서 받은 값. set_* 가 1 인 것만 설정 파일 값을 덮어쓴다. */
typedef struct {
    const char *config_path, *host, *root, *cgi_dir, *name, *tls_cert, *tls_key;
    long port, workers, queue, idle, hdr, maxreq, cgi_ms, https_port;
    int set_port, set_workers, set_queue, set_idle, set_hdr, set_maxreq, set_cgi_ms, set_https_port;
    int verbose;
} cli_t;

static void apply_cli(config_t *c, const cli_t *o)
{
    if (o->host)
        snprintf(c->listen_host, sizeof c->listen_host, "%s", o->host);
    if (o->root)
        snprintf(c->root, sizeof c->root, "%s", o->root);
    if (o->cgi_dir) {
        snprintf(c->cgi_prefix, sizeof c->cgi_prefix, "/cgi-bin");
        snprintf(c->cgi_dir, sizeof c->cgi_dir, "%s", o->cgi_dir);
    }
    if (o->set_port)
        c->listen_port = (int)o->port;
    if (o->set_workers)
        c->workers = o->workers;
    if (o->set_queue)
        c->queue = o->queue;
    if (o->set_idle)
        c->idle_timeout_ms = o->idle;
    if (o->set_hdr)
        c->header_timeout_ms = o->hdr;
    if (o->set_maxreq)
        c->max_requests = o->maxreq;
    if (o->set_cgi_ms)
        c->cgi_timeout_ms = o->cgi_ms;
    if (o->tls_cert)
        snprintf(c->tls_cert, sizeof c->tls_cert, "%s", o->tls_cert);
    if (o->tls_key)
        snprintf(c->tls_key, sizeof c->tls_key, "%s", o->tls_key);
    if (o->set_https_port)
        c->https_port = o->https_port;
}

int main(int argc, char **argv)
{
    /* 큰 배열(PATH_MAX, 백엔드 풀)을 담으므로 스택 대신 정적 저장소.
     * 모두 시작 시 main 스레드가 한 번 쓰고, 워커 생성 이후에는 읽기 전용이다
     * (예외: rproxy 풀의 rr/down 은 원자 변수 — rproxy.h 참고). */
    static config_t cfg;
    static static_ctx_t sctx;
    static cgi_ctx_t cctx;
    static router_t router;
    static rproxy_pool_t pools[CONFIG_MAX_ROUTES];
    static char *args[256];
    static fproxy_ctx_t fctx;
    static char via[64];
    cache_t *cache = NULL;
    const http_limits_t default_limits = HTTP_LIMITS_DEFAULT;
    cli_t cli;
    char err[512];
    int nargs;
    int opt;
    int lfd = -1;
    int tls_lfd = -1;
    struct ssl_ctx_st *tls_ctx = NULL;
    server_listener_t ls[2];
    size_t nls = 0;
    int rc = 1;
    size_t i;
    threadpool_t *tp = NULL;
    conn_config_t ccfg;

    memset(&cli, 0, sizeof cli);
    if (argc >= (int)(sizeof args / sizeof args[0])) {
        fprintf(stderr, "인자가 너무 많다\n");
        return 2;
    }
    nargs = extract_long_name(argc, argv, args, &cli.name);
    if (nargs < 0) {
        fprintf(stderr, "--name 에 값이 필요하다\n");
        return 2;
    }

    while ((opt = getopt(nargs, args, "f:H:p:r:w:q:t:T:m:g:c:n:C:K:s:vh")) != -1) {
        int bad = 0;

        switch (opt) {
        case 'f': cli.config_path = optarg; break;
        case 'H': cli.host = optarg; break;
        case 'r': cli.root = optarg; break;
        case 'g': cli.cgi_dir = optarg; break;
        case 'n': cli.name = optarg; break;
        case 'C': cli.tls_cert = optarg; break;
        case 'K': cli.tls_key = optarg; break;
        case 's': bad = parse_long(optarg, 1, 65535, &cli.https_port); cli.set_https_port = 1; break;
        case 'p': bad = parse_long(optarg, 1, 65535, &cli.port); cli.set_port = 1; break;
        case 'w': bad = parse_long(optarg, 1, 1024, &cli.workers); cli.set_workers = 1; break;
        case 'q': bad = parse_long(optarg, 1, 65536, &cli.queue); cli.set_queue = 1; break;
        case 't': bad = parse_long(optarg, 1, 3600 * 1000, &cli.idle); cli.set_idle = 1; break;
        case 'T': bad = parse_long(optarg, 0, 3600 * 1000, &cli.hdr); cli.set_hdr = 1; break;
        case 'm': bad = parse_long(optarg, 1, 1000000, &cli.maxreq); cli.set_maxreq = 1; break;
        case 'c': bad = parse_long(optarg, 1, 3600 * 1000, &cli.cgi_ms); cli.set_cgi_ms = 1; break;
        case 'v': cli.verbose = 1; break;
        default: usage(args[0]); return opt == 'h' ? 0 : 2;
        }
        if (bad) {
            fprintf(stderr, "잘못된 값: -%c %s\n", opt, optarg);
            usage(args[0]);
            return 2;
        }
    }

    log_init(cli.verbose ? LOG_DEBUG : LOG_INFO, NULL);
    config_defaults(&cfg);
    if (cli.config_path != NULL && config_load(cli.config_path, &cfg, err, sizeof err) != 0) {
        log_msg(LOG_ERROR, "설정 파일 %s: %s", cli.config_path, err);
        return 2;
    }
    apply_cli(&cfg, &cli);
    if (cli.name != NULL)
        http_response_set_backend_name(cli.name); /* 워커 생성 전에 한 번만 */

    if (install_signals() != 0) {
        log_msg(LOG_ERROR, "시그널 설정 실패: %s", strerror(errno));
        return 1;
    }
    if (static_init(&sctx, cfg.root) != 0) {
        log_msg(LOG_ERROR, "문서 루트를 열 수 없음: '%s' (%s)", cfg.root, strerror(errno));
        return 1;
    }

    /* 라우팅: 설정의 route 들, CGI, 그 외는 정적 파일 */
    router_init(&router, static_handle, &sctx);
    if (cfg.cgi_prefix[0] != '\0') {
        if (cgi_init(&cctx, cfg.cgi_dir, cfg.cgi_prefix, (int)cfg.cgi_timeout_ms) != 0) {
            log_msg(LOG_ERROR, "CGI 디렉터리를 열 수 없음: '%s' (%s)", cfg.cgi_dir, strerror(errno));
            return 1;
        }
        if (cctx.dir_len > sctx.root_len && strncmp(cctx.dir, sctx.root, sctx.root_len) == 0 &&
            cctx.dir[sctx.root_len] == '/')
            log_msg(LOG_WARN, "CGI 디렉터리가 문서 루트 안에 있다: 실행 파일이 정적 파일로 노출될 수 있음");
        if (router_add(&router, cfg.cgi_prefix, cgi_handle, &cctx) != 0) {
            log_msg(LOG_ERROR, "CGI 라우트 추가 실패");
            return 1;
        }
        log_msg(LOG_INFO, "CGI: %s/ -> %s (timeout %ldms)", cctx.prefix, cctx.dir,
                cfg.cgi_timeout_ms);
    }
    for (i = 0; i < cfg.nroutes; i++) {
        const config_route_t *r = &cfg.routes[i];
        size_t b;

        if (rproxy_pool_init(&pools[i], r->backends, r->nbackends,
                             (int)cfg.upstream_connect_timeout_ms, (int)cfg.upstream_read_timeout_ms,
                             cfg.upstream_down_secs, err, sizeof err) != 0) {
            log_msg(LOG_ERROR, "route %s: %s", r->prefix, err);
            return 1;
        }
        if (router_add_host(&router, r->host, r->prefix, rproxy_handle, &pools[i]) != 0) {
            log_msg(LOG_ERROR, "route %s 추가 실패 (규칙이 너무 많음)", r->prefix);
            return 1;
        }
        for (b = 0; b < pools[i].n; b++)
            log_msg(LOG_INFO, "route %s%s%s -> %s", r->host[0] ? "host=" : "", r->host,
                    r->prefix, pools[i].backends[b].name);
    }

    /* 이 인스턴스만의 Via 표식: 자기 요청이 돌아오면(프록시 루프) 알아챈다 (upstream.h) */
    snprintf(via, sizeof via, "%s-%lx", UPSTREAM_VIA_DEFAULT, (unsigned long)getpid());
    upstream_set_via(via);
    if (cfg.forward_proxy) {
        if (cfg.cache_max_bytes > 0) {
            cache = cache_create((size_t)cfg.cache_max_bytes, (size_t)cfg.cache_max_object_bytes, 0);
            if (cache == NULL) {
                log_msg(LOG_ERROR, "캐시 생성 실패");
                return 1;
            }
        }
        fctx.cache = cache;
        fctx.connect_timeout_ms = (int)cfg.upstream_connect_timeout_ms;
        fctx.read_timeout_ms = (int)cfg.upstream_read_timeout_ms;
        fctx.default_ttl_ms = cfg.cache_default_ttl_secs * 1000LL;
        router_set_absolute_handler(&router, fproxy_handle, &fctx);
        log_msg(LOG_INFO, "forward proxy: on (cache %s, max %ld bytes, object %ld bytes, ttl %lds)",
                cache ? "on" : "off", cfg.cache_max_bytes, cfg.cache_max_object_bytes,
                cfg.cache_default_ttl_secs);
    }
    log_msg(LOG_INFO, "Via 표식: %s", via);

    /*
     * HTTPS: 인증서와 개인키가 둘 다 있을 때만. 하나만 있으면 설정 실수이므로 시작하지 않는다.
     * SSL_CTX 는 워커를 만들기 전에 여기서 한 번 만들고, 이후 워커들은 읽기만 한다 (tls.h).
     * 인증서·키를 읽지 못하거나 짝이 맞지 않으면 시작을 거부한다 (매 연결마다 실패하는 서버보다 낫다).
     */
    if ((cfg.tls_cert[0] != '\0') != (cfg.tls_key[0] != '\0')) {
        log_msg(LOG_ERROR, "HTTPS 는 인증서(-C/tls_cert)와 개인키(-K/tls_key)가 둘 다 필요하다");
        goto cleanup;
    }
    if (cfg.tls_cert[0] != '\0') {
        if (cfg.https_port == cfg.listen_port) {
            log_msg(LOG_ERROR, "HTTP 와 HTTPS 포트가 같다: %d", cfg.listen_port);
            goto cleanup;
        }
        tls_ctx = tls_ctx_create(cfg.tls_cert, cfg.tls_key);
        if (tls_ctx == NULL)
            goto cleanup; /* 원인은 tls_ctx_create 가 로그로 남겼다 */
    }

    lfd = server_listen(cfg.listen_host, cfg.listen_port, 128);
    if (lfd < 0)
        goto cleanup; /* 원인은 server_listen 이 이미 로그로 남겼다 */
    ls[nls].fd = lfd;
    ls[nls].tls = 0;
    nls++;
    if (tls_ctx != NULL) {
        tls_lfd = server_listen(cfg.listen_host, (int)cfg.https_port, 128);
        if (tls_lfd < 0)
            goto cleanup;
        ls[nls].fd = tls_lfd;
        ls[nls].tls = 1;
        nls++;
    }

    memset(&ccfg, 0, sizeof ccfg);
    ccfg.limits = default_limits;
    ccfg.idle_timeout_ms = (int)cfg.idle_timeout_ms;
    ccfg.header_timeout_ms = (int)cfg.header_timeout_ms;
    ccfg.send_timeout_ms = 10000;
    ccfg.max_requests = (int)cfg.max_requests;
    ccfg.handler = router_handle;
    ccfg.handler_ctx = &router;
    ccfg.stop = &g_stop;
    ccfg.tls_ctx = tls_ctx;

    tp = threadpool_create((size_t)cfg.workers, (size_t)cfg.queue, conn_worker, &ccfg);
    if (tp == NULL) {
        log_msg(LOG_ERROR, "워커 풀 생성 실패");
        goto cleanup;
    }

    log_msg(LOG_INFO, "listening on http://%s:%d  root=%s  workers=%ld queue=%ld idle=%ldms%s%s",
            cfg.listen_host, cfg.listen_port, sctx.root, cfg.workers, cfg.queue,
            cfg.idle_timeout_ms, cli.name ? "  backend-name=" : "", cli.name ? cli.name : "");
    if (tls_ctx != NULL)
        log_msg(LOG_INFO, "listening on https://%s:%ld  cert=%s  (TLS 1.2+)", cfg.listen_host,
                cfg.https_port, cfg.tls_cert);
    rc = server_run(ls, nls, tp, &g_stop) == 0 ? 0 : 1;
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
    if (lfd >= 0 || tls_lfd >= 0) {
        if (lfd >= 0)
            close(lfd);
        if (tls_lfd >= 0)
            close(tls_lfd);
        log_msg(LOG_INFO, "종료 1/4: 리슨 소켓 닫음");
    }
    if (tp != NULL) {
        threadpool_close(tp);
        log_msg(LOG_INFO, "종료 2/4: 작업 큐 닫음, 워커 깨움 (진행 중 요청 마무리 대기)");
        threadpool_join_free(tp);
        tp = NULL;
        log_msg(LOG_INFO, "종료 3/4: 워커 전부 join, 풀 해제");
    }
    if (cache != NULL) { /* 워커가 모두 끝난 뒤에만 해제 (사용 중인 스레드가 없음) */
        cache_stats_t st;

        cache_stats(cache, &st);
        log_msg(LOG_INFO, "cache: hits=%llu misses=%llu stores=%llu evictions=%llu entries=%zu bytes=%zu",
                st.hits, st.misses, st.stores, st.evictions, st.entries, st.bytes);
        cache_destroy(cache);
    }
    /* SSL_CTX 도 워커가 모두 끝난 뒤에만 해제 (연결별 SSL 이 CTX 를 참조하므로) */
    tls_ctx_free(tls_ctx);
    log_msg(LOG_INFO, "종료 4/4: %s", rc == 0 ? "정상 종료" : "오류로 종료");
    return rc;
}
