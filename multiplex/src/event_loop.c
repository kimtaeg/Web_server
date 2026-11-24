/*
 * event_loop.c — 단일 스레드 이벤트 루프와 연결 상태 머신. 설계는 event_loop.h 참고.
 *
 * 읽는 순서 추천: ev_loop_run_once (루프 한 바퀴) → on_readable / on_writable / on_timeout
 *               → drive (상태 머신 본체) → flush (논블로킹 전송).
 */
#include "event_loop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "http_response.h"
#include "log.h"
#include "netio.h"
#include "platform.h"
#include "timer_heap.h"

#define RECV_CHUNK (16 * 1024)   /* 이벤트 한 번에 읽는 최대 바이트 */
#define IO_CHUNK (64 * 1024)     /* 파일 → 소켓 한 조각 */
#define WRITE_BUDGET (256 * 1024) /* 연결 하나가 이벤트 한 번에 보내는 최대 바이트 (공정성) */
#define ACCEPT_BUDGET 64          /* 이벤트 한 번에 accept 하는 최대 연결 수 (공정성) */
#define MAX_EVENTS 256
#define STOP_CHECK_MS 500         /* 이벤트가 없어도 이 주기로 깨어나 종료 플래그 확인 */
#define LINGER_MS 200             /* 닫기 전 남은 입력을 비우는 최대 대기 */
#define LINGER_MAX (64 * 1024)

typedef enum { C_READ, C_WRITE, C_LINGER } cstate_t;

typedef struct {
    int fd;
    cstate_t st;
    char ip[INET6_ADDRSTRLEN];

    /* 요청 수신 */
    http_parser_t parser;
    buf_t in;
    long long req_start_ms; /* 현재 요청의 첫 바이트 도착 시각, 아직 없으면 -1 (헤더 마감 기준) */
    long long last_io_ms;   /* 마지막으로 바이트를 주고받은 시각 (유휴/전송 타임아웃 기준) */
    int nreq;

    /* 응답 송신: out[out_off..] 를 보내고, 다 보내면 file 에서 다음 조각을 읽어 out 에 채운다 */
    buf_t out;
    size_t out_off;
    int file_fd;
    size_t file_left;
    int close_after; /* 이 응답 뒤에 연결을 닫는다 */
    size_t req_used; /* 응답이 끝나면 in 에서 덜어낼 바이트 (요청 경계를 잃은 오류 응답이면 0) */

    /* 접근 로그: target 은 in 안을 가리킨다. 응답 중에는 읽기를 멈추므로 in 이 바뀌지 않아 안전하다 */
    const char *log_method;
    const char *log_target;
    size_t log_target_len;
    int log_vminor;
    int log_status;
    size_t log_bytes;
    long long log_t0;

    int want_read, want_write; /* poller 에 등록된 관심 (바뀔 때만 시스템 콜) */
    size_t linger_drained;
    size_t timer_slot; /* timer_heap 위치 */
} conn_t;

struct ev_loop {
    ev_config_t cfg;
    int pfd;        /* epoll/kqueue fd */
    int lfd;        /* 리슨 소켓 (-1 가능) */
    int reserve_fd; /* EMFILE 대비 예비 fd (아래 accept_emfile 참고) */
    conn_t **conns; /* fd → 연결. fd 는 작은 정수라 배열이 해시맵보다 빠르고 단순하다 */
    size_t conns_cap;
    size_t nconns;
    timer_heap_t timers;
    ev_stats_t stats;
    int draining;
    long long drain_deadline_ms;
    char io[IO_CHUNK]; /* 공용 임시 버퍼: 스레드가 하나라 연결마다 둘 필요가 없다 */
};

/* 상태 머신 진행 결과: 연결이 아직 살아 있나 */
enum { ALIVE = 0, CLOSED = -1 };

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static int stopping(const ev_loop_t *l)
{
    return l->draining || (l->cfg.stop != NULL && *l->cfg.stop);
}

static int set_nonblock_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL);

    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
        return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* 접근 로그용 상대 IP. IPv4/IPv6 가 아니면(테스트의 socketpair 등) "-". */
static void peer_ip(int fd, char *out, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    const void *addr = NULL;

    snprintf(out, n, "-");
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
        return;
    if (ss.ss_family == AF_INET)
        addr = &((struct sockaddr_in *)&ss)->sin_addr;
    else if (ss.ss_family == AF_INET6)
        addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
    if (addr == NULL || inet_ntop(ss.ss_family, addr, out, (socklen_t)n) == NULL)
        snprintf(out, n, "-");
}

/* ------------------------------------------------------------------ */
/* 연결 생성/해제, 관심/타이머 등록                                      */
/* ------------------------------------------------------------------ */

static conn_t *lookup(const ev_loop_t *l, int fd)
{
    return (fd >= 0 && (size_t)fd < l->conns_cap) ? l->conns[fd] : NULL;
}

static void conn_close(ev_loop_t *l, conn_t *c)
{
    timer_heap_remove(&l->timers, &c->timer_slot);
    (void)plat_poller_del(l->pfd, c->fd);
    close(c->fd);
    if (c->file_fd >= 0)
        close(c->file_fd);
    buf_free(&c->in);
    buf_free(&c->out);
    l->conns[c->fd] = NULL;
    l->nconns--;
    free(c);
}

/* poller 관심을 바꾼다. 실제로 달라질 때만 시스템 콜. 0 / -1 */
static int set_interest(ev_loop_t *l, conn_t *c, int want_read, int want_write)
{
    if (c->want_read == want_read && c->want_write == want_write)
        return 0;
    if (plat_poller_mod(l->pfd, c->fd, want_read, want_write) != 0)
        return -1;
    c->want_read = want_read;
    c->want_write = want_write;
    return 0;
}

/* 현재 상태에 맞는 마감을 계산해 타이머에 건다. 0 / -1(메모리 부족) */
static int arm_timer(ev_loop_t *l, conn_t *c)
{
    long long d;

    switch (c->st) {
    case C_READ:
        d = c->last_io_ms + l->cfg.idle_timeout_ms;
        /* 헤더를 받는 중이면, 다시 시작되지 않는 헤더 마감도 함께 (짧은 쪽) */
        if (c->req_start_ms >= 0 && l->cfg.header_timeout_ms > 0 &&
            !http_parser_headers_done(&c->parser) &&
            c->req_start_ms + l->cfg.header_timeout_ms < d)
            d = c->req_start_ms + l->cfg.header_timeout_ms;
        break;
    case C_WRITE:
        d = c->last_io_ms + l->cfg.send_timeout_ms;
        break;
    default: /* C_LINGER */
        d = c->last_io_ms + LINGER_MS;
        break;
    }
    return timer_heap_set(&l->timers, d, c, &c->timer_slot);
}

/* 요청 버퍼 상한: threadpool/ conn.c 와 같다 (요청 최대 크기 + 파이프라이닝으로 함께 온 조각) */
static size_t in_cap(const ev_loop_t *l)
{
    return l->cfg.limits.max_uri + l->cfg.limits.max_header_bytes + l->cfg.limits.max_body +
           2 * RECV_CHUNK;
}

static int grow_table(ev_loop_t *l, int fd)
{
    size_t ncap = l->conns_cap ? l->conns_cap : 64;
    conn_t **nt;

    while (ncap <= (size_t)fd)
        ncap *= 2;
    nt = realloc(l->conns, ncap * sizeof *nt);
    if (nt == NULL)
        return -1;
    memset(nt + l->conns_cap, 0, (ncap - l->conns_cap) * sizeof *nt);
    l->conns = nt;
    l->conns_cap = ncap;
    return 0;
}

/* fd 의 소유권을 가져간다. 실패하면 닫는다. */
static int conn_add(ev_loop_t *l, int fd)
{
    conn_t *c = NULL;

    if ((size_t)fd >= l->conns_cap && grow_table(l, fd) != 0)
        goto fail;
    c = calloc(1, sizeof *c);
    if (c == NULL)
        goto fail;
    c->fd = fd;
    c->st = C_READ;
    c->file_fd = -1;
    c->req_start_ms = -1;
    c->last_io_ms = now_ms();
    c->timer_slot = TIMER_NONE;
    peer_ip(fd, c->ip, sizeof c->ip);
    http_parser_init(&c->parser, &l->cfg.limits);
    buf_init(&c->in, 0, in_cap(l)); /* 0: 첫 바이트가 올 때 할당 (유휴 연결의 메모리를 줄인다) */
    buf_init(&c->out, 0, STATIC_HEAD_MAX + IO_CHUNK);
    if (plat_poller_add(l->pfd, fd, 1, 0) != 0)
        goto fail;
    c->want_read = 1;
    l->conns[fd] = c;
    l->nconns++;
    if (l->nconns > l->stats.peak_conns)
        l->stats.peak_conns = l->nconns;
    if (arm_timer(l, c) != 0) {
        conn_close(l, c);
        return -1;
    }
    return 0;

fail:
    if (c != NULL) {
        buf_free(&c->in);
        buf_free(&c->out);
        free(c);
    }
    close(fd);
    return -1;
}

/* ------------------------------------------------------------------ */
/* 응답 준비                                                            */
/* ------------------------------------------------------------------ */

/* 파일 다음 조각을 out 뒤에 붙인다. 반환: 붙인 바이트 수(0 = 파일이 줄어듦), -1 = 버퍼 오류. */
static ssize_t refill(ev_loop_t *l, conn_t *c)
{
    size_t want = c->file_left < IO_CHUNK ? c->file_left : IO_CHUNK;
    ssize_t r;

    /* 디스크 read 는 논블로킹이 아니다 (일반 파일에 O_NONBLOCK 은 효과가 없다).
     * 페이지 캐시에 있으면 메모리 복사라 빠르지만, 디스크까지 가면 그동안 루프 전체가 멈춘다.
     * nginx 는 이 때문에 aio/스레드 풀 옵션을 둔다 (DESIGN.md 16절). */
    do {
        r = read(c->file_fd, l->io, want);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
        /* 전송 중 파일이 줄어듦: 약속한 Content-Length 를 채울 수 없으니 연결을 끊어 알린다 */
        close(c->file_fd);
        c->file_fd = -1;
        c->file_left = 0;
        c->close_after = 1;
        return 0;
    }
    if (buf_append(&c->out, l->io, (size_t)r) != BUF_OK)
        return -1;
    c->file_left -= (size_t)r;
    if (c->file_left == 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    return r;
}

static void begin_write(conn_t *c, int status)
{
    c->st = C_WRITE;
    c->out_off = 0;
    c->log_status = status;
    c->log_bytes = 0;
    c->log_t0 = now_ms();
    c->last_io_ms = c->log_t0; /* 전송 타임아웃은 응답 시작부터 센다 */
}

/* 요청 경계를 잃었거나(파싱 오류) 마감을 넘긴 경우의 오류 응답. 항상 연결을 닫는다. */
static int respond_status(conn_t *c, int status)
{
    buf_reset(&c->out);
    if (http_response_build_status_page(&c->out, status, 0, 0, NULL) != BUF_OK)
        return -1;
    c->close_after = 1;
    c->req_used = 0;
    c->log_method = NULL;
    c->log_target = NULL;
    c->log_target_len = 0;
    c->log_vminor = 0;
    begin_write(c, status);
    return 0;
}

static int respond_request(ev_loop_t *l, conn_t *c, const http_request_t *req, size_t used)
{
    static_plan_t plan;
    int ka;
    int rc = -1;

    c->nreq++;
    l->stats.requests++;
    ka = req->keep_alive && c->nreq < l->cfg.max_requests && !stopping(l);
    static_plan_init(&plan);
    if (static_prepare(l->cfg.sctx, req, ka, &plan) != 0)
        goto cleanup;
    buf_reset(&c->out);
    if (buf_append(&c->out, plan.head.data, plan.head.len) != BUF_OK)
        goto cleanup;
    c->file_fd = plan.file_fd; /* 소유권을 연결로 옮긴다 */
    c->file_left = plan.file_len;
    plan.file_fd = -1;
    c->close_after = !ka;
    c->req_used = used;
    c->log_method = http_method_name(req->method);
    c->log_target = req->target;
    c->log_target_len = req->target_len;
    c->log_vminor = req->version_minor;
    begin_write(c, plan.status);
    /*
     * 본문 첫 조각을 헤더와 같은 버퍼에 붙여 send 한 번으로 보낸다 (작은 파일은 응답 전체가 한 번에).
     * 헤더와 본문을 따로 보내면 Nagle + 지연 ACK 로 두 번째 조각이 늦어질 수 있다 (threadpool/ 과 같은 이유).
     */
    if (c->file_left > 0 && refill(l, c) < 0)
        goto cleanup;
    rc = 0;

cleanup:
    static_plan_free(&plan);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 전송                                                                */
/* ------------------------------------------------------------------ */

enum { FLUSH_DONE, FLUSH_BLOCKED, FLUSH_ERR };

/*
 * out 을 보내고, 다 보내면 파일에서 다음 조각을 채워 계속 보낸다.
 * EAGAIN(소켓 송신 버퍼가 가득) 이면 FLUSH_BLOCKED: 쓰기 가능 이벤트가 오면 여기서부터 이어간다.
 * WRITE_BUDGET 을 다 써도 FLUSH_BLOCKED: 큰 파일을 빠른 클라이언트에 보내는 연결 하나가
 * 루프를 독점해 다른 연결이 굶지 않도록 (소켓은 여전히 쓰기 가능이라 LT 가 곧 다시 깨워 준다).
 */
static int flush(ev_loop_t *l, conn_t *c)
{
    size_t budget = WRITE_BUDGET;

    for (;;) {
        size_t n;
        ssize_t w;

        if (c->out_off == c->out.len) {
            if (c->file_left == 0)
                return FLUSH_DONE;
            if (budget == 0)
                return FLUSH_BLOCKED;
            buf_reset(&c->out);
            c->out_off = 0;
            if (refill(l, c) < 0)
                return FLUSH_ERR;
            continue;
        }
        if (budget == 0)
            return FLUSH_BLOCKED;
        n = c->out.len - c->out_off;
        if (n > budget)
            n = budget;
        w = netio_send_nb(c->fd, c->out.data + c->out_off, n);
        if (w > 0) {
            c->out_off += (size_t)w;
            c->log_bytes += (size_t)w;
            budget -= (size_t)w;
            c->last_io_ms = now_ms();
            continue;
        }
        if (w == NETIO_AGAIN)
            return FLUSH_BLOCKED;
        return FLUSH_ERR; /* EPIPE, ECONNRESET 등: 상대가 떠났다 */
    }
}

static void log_response(const conn_t *c)
{
    log_access(c->ip, c->log_method, c->log_target, c->log_target_len, c->log_vminor,
               c->log_status, c->log_bytes, (long)(now_ms() - c->log_t0));
}

/* 쓰기 방향만 닫고 남은 입력을 잠깐 비운 뒤 닫는다 (threadpool/ conn.c 의 linger_close 와 같은 이유). */
static void begin_linger(conn_t *c)
{
    (void)shutdown(c->fd, SHUT_WR);
    c->st = C_LINGER;
    c->linger_drained = 0;
    c->last_io_ms = now_ms();
}

/* 응답을 다 보냈다. keep-alive 면 다음 요청으로, 아니면 LINGER 로. */
static void finish_response(ev_loop_t *l, conn_t *c)
{
    log_response(c);
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    c->file_left = 0;
    if (c->close_after || stopping(l)) {
        begin_linger(c);
        return;
    }
    buf_consume(&c->in, c->req_used); /* 처리한 요청만 덜어내고 남은 바이트(다음 요청)는 보존 */
    c->log_target = NULL;             /* consume 으로 무효가 된 포인터를 남기지 않는다 */
    http_parser_init(&c->parser, &l->cfg.limits);
    c->req_start_ms = c->in.len > 0 ? now_ms() : -1;
    c->st = C_READ;
    /* 유휴 연결의 메모리를 줄인다: 비어 있는 버퍼는 반납 (수천 개 유휴 연결에서 차이가 크다) */
    if (c->in.len == 0) {
        size_t max = c->in.max;

        buf_free(&c->in);
        buf_init(&c->in, 0, max);
    }
    {
        size_t max = c->out.max;

        buf_free(&c->out);
        buf_init(&c->out, 0, max);
    }
}

/* ------------------------------------------------------------------ */
/* 상태 머신                                                            */
/* ------------------------------------------------------------------ */

/*
 * 더 진행할 수 없을 때(입력이 더 필요 / 송신 버퍼가 가득)까지 상태 머신을 돌린다.
 * 버퍼에 이미 다음 요청이 있으면(파이프라이닝) 새 이벤트 없이 이어서 처리한다 — LT 라도 소켓에
 * 새 데이터가 없으면 다시 깨워 주지 않으므로, 여기서 끝까지 처리해야 요청이 멈춰 있지 않는다.
 * 반환 ALIVE / CLOSED (CLOSED 면 c 는 이미 해제됨).
 */
static int drive(ev_loop_t *l, conn_t *c)
{
    for (;;) {
        switch (c->st) {
        case C_READ: {
            http_request_t req;
            size_t used = 0;
            parse_result_t r;

            if (stopping(l)) /* 종료 중에는 새 요청을 시작하지 않는다 */
                goto close;
            r = http_parse(&c->parser, c->in.data, c->in.len, &req, &used);
            if (r == PARSE_NEED_MORE) {
                if (set_interest(l, c, 1, 0) != 0 || arm_timer(l, c) != 0)
                    goto close;
                return ALIVE;
            }
            if (r == PARSE_ERROR) {
                /* 요청 경계를 잃었으므로 이 연결로는 더 진행할 수 없다 → 오류 응답 후 close */
                if (respond_status(c, http_parser_error_status(&c->parser)) != 0)
                    goto close;
                break;
            }
            if (respond_request(l, c, &req, used) != 0)
                goto close;
            break;
        }
        case C_WRITE:
            switch (flush(l, c)) {
            case FLUSH_DONE:
                finish_response(l, c);
                break;
            case FLUSH_BLOCKED:
                /* 응답 중에는 읽지 않는다: 다음 요청은 커널 버퍼에 남겨 두면 된다 (메모리 상한 + 순서 보장) */
                if (set_interest(l, c, 0, 1) != 0 || arm_timer(l, c) != 0)
                    goto close;
                return ALIVE;
            default:
                log_response(c); /* 보낸 만큼이라도 기록 */
                goto close;
            }
            break;
        default: /* C_LINGER */
            if (set_interest(l, c, 1, 0) != 0 || arm_timer(l, c) != 0)
                goto close;
            return ALIVE;
        }
    }

close:
    conn_close(l, c);
    return CLOSED;
}

static void on_readable(ev_loop_t *l, conn_t *c)
{
    ssize_t n;

    if (c->st == C_WRITE) /* 읽기 관심을 끈 상태: 같은 배치에 남은 이전 이벤트 (무시) */
        return;
    if (c->st == C_LINGER) {
        for (;;) {
            n = netio_recv_nb(c->fd, l->io, sizeof l->io);
            if (n > 0 && (c->linger_drained += (size_t)n) < LINGER_MAX)
                continue;
            if (n == NETIO_AGAIN)
                return; /* 아직 열려 있음: 타이머가 정리한다 */
            conn_close(l, c); /* EOF, 오류, 너무 많이 버림 */
            return;
        }
    }

    /* C_READ: 이벤트 한 번에 한 번만 읽는다. LT 라서 남은 데이터가 있으면 다음 wait 에서 또 알려 준다.
     * 한 연결이 계속 데이터를 보내도 다른 연결과 번갈아 처리된다 (공정성). */
    n = netio_recv_nb(c->fd, l->io, RECV_CHUNK);
    if (n == NETIO_AGAIN)
        return; /* 가짜 깨움 (fd 재사용 직후의 이전 이벤트 등): 무해 */
    if (n <= 0) {
        conn_close(l, c); /* EOF 또는 오류. 요청 중간이면 응답할 상대가 없다 */
        return;
    }
    c->last_io_ms = now_ms();
    if (c->req_start_ms < 0)
        c->req_start_ms = c->last_io_ms;
    if (buf_append(&c->in, l->io, (size_t)n) != BUF_OK) {
        /* 파서가 크기 한도(413/431)를 먼저 잡으므로 여기는 메모리 부족 */
        if (respond_status(c, 500) != 0) {
            conn_close(l, c);
            return;
        }
    }
    (void)drive(l, c);
}

static void on_writable(ev_loop_t *l, conn_t *c)
{
    if (c->st == C_WRITE)
        (void)drive(l, c);
}

static void on_timeout(ev_loop_t *l, conn_t *c)
{
    long long now = now_ms();

    l->stats.timeouts++;
    if (c->st == C_READ && c->req_start_ms >= 0 && l->cfg.header_timeout_ms > 0 &&
        !http_parser_headers_done(&c->parser) &&
        now - c->req_start_ms >= l->cfg.header_timeout_ms) {
        /* slowloris: 헤더를 마감 안에 다 보내지 못함 → 408 후 종료 */
        if (respond_status(c, 408) == 0) {
            (void)drive(l, c);
            return;
        }
    }
    /* 유휴(요청 사이 또는 요청 중 정체), 전송 정체, LINGER 만료: 그냥 닫는다 */
    conn_close(l, c);
}

/* ------------------------------------------------------------------ */
/* accept                                                              */
/* ------------------------------------------------------------------ */

/* 동시 연결 상한 초과: 503 을 한 번만, 블록하지 않고 보내 본 뒤 닫는다 (threadpool/ 의 큐 포화와 같은 응답). */
static void reject_busy(int fd)
{
    buf_t b;

    buf_init(&b, 512, 4096);
    if (http_response_build_status_page(&b, 503, 0, 0, "Retry-After: 1\r\n") == BUF_OK)
        (void)netio_send_nb(fd, b.data, b.len); /* 논블로킹 소켓: 못 보내면 그만 */
    buf_free(&b);
    close(fd);
}

/*
 * fd 고갈(EMFILE/ENFILE): accept 가 실패해도 연결은 커널 backlog 에 그대로 남아 있으므로
 * 리슨 소켓이 계속 readable 로 깨어나 루프가 100% CPU 로 헛돈다 (LT 의 함정).
 * 미리 잡아 둔 예비 fd 를 잠깐 내놓고 그 자리로 accept → 즉시 close 해서 backlog 에서 빼낸다.
 * (libev/nginx 등이 쓰는 기법. 클라이언트는 응답 없이 끊기지만 서버는 계속 산다.)
 */
static void accept_emfile(ev_loop_t *l)
{
    int fd;

    l->stats.rejected_emfile++;
    if ((l->stats.rejected_emfile & (l->stats.rejected_emfile - 1)) == 0) /* 1,2,4,8.. 번째만 로그 */
        log_msg(LOG_WARN, "fd 고갈(EMFILE): 새 연결을 닫음 (누적 %llu, ulimit -n 확인)",
                l->stats.rejected_emfile);
    if (l->reserve_fd < 0)
        return;
    close(l->reserve_fd);
    fd = accept(l->lfd, NULL, NULL);
    if (fd >= 0)
        close(fd);
    l->reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
}

static void do_accept(ev_loop_t *l)
{
    int i;

    for (i = 0; i < ACCEPT_BUDGET; i++) {
        int one = 1;
        int fd = accept(l->lfd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                accept_emfile(l);
                return;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_msg(LOG_WARN, "accept 실패: %s", strerror(errno));
            return; /* EAGAIN: backlog 를 다 비웠다 */
        }
        /* accept 된 소켓이 O_NONBLOCK 을 물려받는지는 OS 마다 다르다 (BSD 는 상속, Linux 는 아님) → 명시 */
        if (set_nonblock_cloexec(fd) != 0 || platform_sock_init(fd) != 0) {
            close(fd);
            continue;
        }
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (l->nconns >= (size_t)l->cfg.max_conns || stopping(l)) {
            l->stats.rejected_busy++;
            reject_busy(fd);
            continue;
        }
        l->stats.accepted++;
        (void)conn_add(l, fd);
    }
}

/* ------------------------------------------------------------------ */
/* 공개 API                                                            */
/* ------------------------------------------------------------------ */

ev_loop_t *ev_loop_create(const ev_config_t *cfg, int lfd)
{
    ev_loop_t *l = calloc(1, sizeof *l);

    if (l == NULL)
        return NULL;
    l->cfg = *cfg;
    l->lfd = lfd;
    l->reserve_fd = -1;
    timer_heap_init(&l->timers);
    l->pfd = plat_poller_create();
    if (l->pfd < 0)
        goto fail;
    if (lfd >= 0) {
        if (set_nonblock_cloexec(lfd) != 0 || plat_poller_add(l->pfd, lfd, 1, 0) != 0)
            goto fail;
        l->reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    return l;

fail:
    if (l->pfd >= 0)
        close(l->pfd);
    free(l);
    return NULL;
}

void ev_loop_destroy(ev_loop_t *l)
{
    size_t i;

    if (l == NULL)
        return;
    for (i = 0; i < l->conns_cap; i++)
        if (l->conns[i] != NULL)
            conn_close(l, l->conns[i]);
    free(l->conns);
    timer_heap_free(&l->timers);
    if (l->reserve_fd >= 0)
        close(l->reserve_fd);
    close(l->pfd);
    free(l);
}

int ev_loop_add_conn(ev_loop_t *l, int fd)
{
    if (set_nonblock_cloexec(fd) != 0) {
        close(fd);
        return -1;
    }
    return conn_add(l, fd);
}

int ev_loop_run_once(ev_loop_t *l, int max_wait_ms)
{
    plat_event_t evs[MAX_EVENTS];
    long long now = now_ms();
    long long next = timer_heap_peek(&l->timers, NULL);
    int wait = max_wait_ms;
    int n, i;
    void *owner;

    if (next >= 0) { /* 가장 이른 타이머 마감까지만 잔다 */
        long long d = next - now;

        if (d < 0)
            d = 0;
        if (d < wait)
            wait = (int)d;
    }
    n = plat_poller_wait(l->pfd, evs, MAX_EVENTS, wait);
    if (n < 0) {
        log_msg(LOG_ERROR, "%s wait 실패: %s", PLATFORM_POLLER_NAME, strerror(errno));
        return -1;
    }
    for (i = 0; i < n; i++) {
        conn_t *c;

        if (evs[i].fd == l->lfd && l->lfd >= 0) {
            if (!l->draining)
                do_accept(l);
            continue;
        }
        /* 같은 배치 안에서 앞 이벤트 처리 중 이 연결이 닫혔을 수 있으므로 매번 다시 찾는다.
         * 닫힌 fd 번호가 accept 로 재사용됐다면 이전 연결의 이벤트가 새 연결로 갈 수 있는데,
         * 그 경우 recv/send 가 EAGAIN 을 돌려줄 뿐이라 무해하다. */
        c = lookup(l, evs[i].fd);
        if (c != NULL && evs[i].readable)
            on_readable(l, c);
        c = lookup(l, evs[i].fd);
        if (c != NULL && evs[i].writable)
            on_writable(l, c);
    }

    /* 만료된 타이머. on_timeout 이 같은 연결에 새 마감(미래)을 걸 수 있지만 now 이후라 이 루프는 끝난다 */
    now = now_ms();
    while ((next = timer_heap_peek(&l->timers, &owner)) >= 0 && next <= now) {
        conn_t *c = owner;

        timer_heap_remove(&l->timers, &c->timer_slot);
        on_timeout(l, c);
    }
    return 0;
}

/* 종료 시작: accept 중단, 요청을 기다리던(응답 중이 아닌) 연결은 바로 닫는다. */
static void start_drain(ev_loop_t *l)
{
    size_t i, closed = 0;

    l->draining = 1;
    l->drain_deadline_ms = now_ms() + l->cfg.shutdown_grace_ms;
    if (l->lfd >= 0)
        (void)plat_poller_del(l->pfd, l->lfd);
    for (i = 0; i < l->conns_cap; i++) {
        conn_t *c = l->conns[i];

        if (c != NULL && c->st == C_READ) {
            conn_close(l, c);
            closed++;
        }
    }
    log_msg(LOG_INFO, "종료 1/3: accept 중단, 대기 연결 %zu개 닫음, 응답 중 %zu개 마무리 대기", closed,
            l->nconns);
}

int ev_loop_run(ev_loop_t *l)
{
    for (;;) {
        if (!l->draining && l->cfg.stop != NULL && *l->cfg.stop)
            start_drain(l);
        if (l->draining) {
            if (l->nconns == 0)
                break;
            if (now_ms() >= l->drain_deadline_ms) {
                log_msg(LOG_WARN, "종료 대기 시간 초과: 연결 %zu개를 강제로 닫음", l->nconns);
                break;
            }
        }
        if (ev_loop_run_once(l, STOP_CHECK_MS) != 0)
            return -1;
    }
    return 0;
}

size_t ev_loop_nconns(const ev_loop_t *l)
{
    return l->nconns;
}

void ev_loop_stats(const ev_loop_t *l, ev_stats_t *out)
{
    *out = l->stats;
}
