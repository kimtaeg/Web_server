/*
 * event_loop.h — 단일 스레드 이벤트 루프 (I/O 멀티플렉싱). threadpool/ 의 server + threadpool + conn 을 대신한다.
 *
 * [구조]
 *   while (!끝) {
 *       n = plat_poller_wait(다음 타이머 마감까지);   ← 준비된 소켓만 돌려받는다
 *       for 이벤트: 리슨 소켓이면 accept, 연결이면 그 연결의 상태 머신을 한 걸음 진행
 *       만료된 타이머 처리 (유휴 → 닫기, 헤더 마감 → 408, 전송 정체 → 닫기)
 *   }
 * 스레드가 하나뿐이라 어떤 호출도 블록하면 안 된다: 모든 소켓은 O_NONBLOCK, EAGAIN 이면 그 자리에서
 * 멈추고(상태를 연결 구조체에 남기고) 다음 이벤트로 넘어간다.
 *
 * [연결 상태 머신]
 *   READ  ──요청 완성──▶ WRITE ──다 보냄──▶ keep-alive: READ (버퍼에 남은 다음 요청을 바로 파싱)
 *    │                    │                 아니면    : LINGER
 *    ├─ 파싱 오류 → 오류 응답 WRITE → LINGER
 *    ├─ 헤더 마감 초과 → 408 WRITE → LINGER
 *    └─ EOF / 유휴 타임아웃 → 닫기
 *   LINGER: 쓰기 방향만 닫고(FIN) 남은 입력을 잠깐 버린 뒤 닫는다 (RST 로 응답이 버려지는 것 방지)
 *
 * [threadpool/ 과 같은 정책] 오류 코드, keep-alive/최대 요청 수, 헤더 마감 408, 파이프라이닝 순서, 접근 로그 형식.
 * [다른 점] 동시 연결 수가 워커 수가 아니라 -c(최대 연결 수)와 fd 한도로만 제한된다.
 *           대신 요청 처리 중 블록하는 일(디스크 read)이 있으면 그동안 모든 연결이 멈춘다 (DESIGN.md 16절).
 *
 * 동기화: 모든 상태를 이벤트 루프 스레드 하나만 만진다. 락이 없다.
 *         시그널 핸들러와 공유하는 것은 *stop (volatile sig_atomic_t) 하나뿐.
 */
#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <signal.h>
#include <stddef.h>

#include "http_parser.h"
#include "static.h"

typedef struct {
    http_limits_t limits;
    int idle_timeout_ms;   /* 마지막으로 바이트를 받은 뒤 다음 바이트를 기다리는 최대 시간 */
    int header_timeout_ms; /* 요청 첫 바이트 → 헤더 끝 총 시간. 0 이면 끔. 초과 → 408 (slowloris) */
    int send_timeout_ms;   /* 응답 전송이 이 시간 동안 한 바이트도 진척이 없으면 닫는다 */
    int max_requests;      /* 연결당 최대 요청 수. 마지막 응답에 Connection: close */
    int max_conns;         /* 동시 연결 상한. 넘으면 503 후 닫기 */
    int shutdown_grace_ms; /* 종료 시 진행 중 응답을 기다리는 최대 시간 */
    const static_ctx_t *sctx;
    volatile sig_atomic_t *stop; /* NULL 가능 */
} ev_config_t;

typedef struct {
    unsigned long long accepted;
    unsigned long long rejected_busy; /* max_conns 초과로 503 */
    unsigned long long rejected_emfile; /* fd 고갈로 즉시 닫음 */
    unsigned long long requests;
    unsigned long long timeouts; /* 유휴/헤더/전송 타임아웃으로 닫힘 */
    size_t peak_conns;
} ev_stats_t;

typedef struct ev_loop ev_loop_t;

/*
 * lfd: 리슨 소켓 (논블로킹으로 바꿔 등록한다. 소유권은 호출자). -1 이면 리슨 없음(단위 테스트용).
 * 실패 시 NULL.
 */
ev_loop_t *ev_loop_create(const ev_config_t *cfg, int lfd);

/* 남은 연결을 모두 닫고 해제한다. lfd 는 닫지 않는다. */
void ev_loop_destroy(ev_loop_t *l);

/* 이미 accept 된 소켓을 넘긴다(소유권 이전, 실패해도 닫힌다). 테스트에서 socketpair 를 넣는 용도. 0/-1 */
int ev_loop_add_conn(ev_loop_t *l, int fd);

/* 이벤트를 한 번 기다려 처리한다(최대 max_wait_ms). 반환 0, 복구 불가 오류 -1. */
int ev_loop_run_once(ev_loop_t *l, int max_wait_ms);

/*
 * *stop 이 설정될 때까지 돌고, 설정되면 graceful shutdown:
 *   accept 중단 → 요청을 기다리던 연결은 바로 닫기 → 보내던 응답은 끝까지(최대 shutdown_grace_ms) → 반환.
 * 반환 0, 복구 불가 오류 -1.
 */
int ev_loop_run(ev_loop_t *l);

size_t ev_loop_nconns(const ev_loop_t *l);
void ev_loop_stats(const ev_loop_t *l, ev_stats_t *out);

#endif /* EVENT_LOOP_H */
