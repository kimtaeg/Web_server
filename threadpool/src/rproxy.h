/*
 * rproxy.h — 리버스 프록시: 백엔드 풀 + 라운드로빈 + 간이 헬스(down 표시).
 *
 * [선택] start = atomic_fetch_add(&rr, 1) % n 에서 시작해, down 이 아닌 첫 백엔드.
 * [실패] 연결 실패(거부/타임아웃)면 그 백엔드를 down_secs 동안 down 으로 표시하고
 *        다른 백엔드로 1회만 재시도한다.
 *        재시도는 "연결 단계 실패"에서만 한다: 요청을 보낸 뒤의 실패를 재시도하면 POST 가
 *        두 번 실행될 수 있다(비멱등). 연결 전이면 백엔드는 요청을 본 적이 없으므로 안전하다.
 * [상태] 전부 down → 시도 없이 즉시 502. 마지막 실패가 연결 타임아웃 → 504, 그 외 연결 실패 → 502.
 *        연결 후 응답 대기 중 읽기 타임아웃 → 504.
 *
 * [동기화] 락 없음. 여러 워커가 동시에 쓰는 값은 둘뿐이고 둘 다 C11 원자 변수다.
 *   rr            : atomic_fetch_add — 동시에 호출해도 각자 다른 값을 받아 정확히 고르게 분배
 *   down_until_ms : atomic_load/store — 단일 값이라 쓰기 경쟁이 나도 "어느 한 시각"이 남을 뿐
 *                   (둘 다 거의 같은 미래 시각이므로 결과가 같다). 읽는 쪽은 찢어진 값을 보지 않는다.
 *   나머지 필드(주소, 타임아웃)는 시작 시 한 번 쓰고 이후 읽기 전용.
 */
#ifndef RPROXY_H
#define RPROXY_H

#include <stdatomic.h>
#include <stddef.h>
#include <sys/socket.h>

#include "config.h"
#include "conn.h"

typedef struct {
    char name[CONFIG_MAX_NAME + 8]; /* "host:port" — 로그, Host 대체값 */
    struct sockaddr_storage addr;
    socklen_t addrlen;
    _Atomic long long down_until_ms; /* 단조 시계 ms. 이 시각 전에는 down. 0 = 정상 */
} backend_t;

typedef struct {
    backend_t backends[CONFIG_MAX_BACKENDS];
    size_t n;
    _Atomic unsigned rr;
    int connect_timeout_ms;
    int read_timeout_ms;
    long long down_ms;
} rproxy_pool_t;

/* 백엔드 주소를 getaddrinfo 로 미리 푼다 (요청마다 DNS 조회하지 않음). 실패 시 err 에 이유. */
int rproxy_pool_init(rproxy_pool_t *p, const config_hostport_t *hps, size_t n, int connect_ms,
                     int read_ms, long down_secs, char *err, size_t errsz);

long long rproxy_now_ms(void);

/*
 * now 기준으로 down 이 아닌 백엔드를 RR 로 고른다. exclude 인덱스는 건너뛴다(-1 이면 없음).
 * 반환: 인덱스, 고를 수 없으면 -1. (now 를 인자로 받아 시간을 흉내 내는 테스트가 가능하다)
 */
int rproxy_pick(rproxy_pool_t *p, long long now_ms, int exclude);

void rproxy_mark_down(rproxy_pool_t *p, size_t i, long long now_ms);

/* conn_handler_fn. ctx 는 rproxy_pool_t*. */
int rproxy_handle(void *ctx, conn_t *cl, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res);

#endif /* RPROXY_H */
