/*
 * threadpool.h — 고정 N 워커 + bounded queue (원형 버퍼).
 *
 * [왜 고정 워커 + bounded queue 인가]
 * 연결마다 스레드를 만들면 접속 폭주 시 스레드 수가 무한히 늘어 메모리/스케줄링이 붕괴한다.
 * 워커 수를 고정하고 대기열 길이에도 상한을 두면, 감당 못 할 부하는 accept 직후 503 으로
 * 빠르게 거절된다(load shedding). 이미 받은 연결의 응답 지연이 무한히 늘어나는 것보다 낫다.
 *
 * [동기화: mutex 1 + cond 2]
 *   mu        : q, head, count, shutting_down 을 보호
 *   not_empty : 워커가 "할 일이 생길 때까지" 기다린다 (생산자가 push 후 signal)
 *   not_full  : threadpool_submit_wait 가 "자리가 날 때까지" 기다린다 (워커가 pop 후 signal)
 * 조건이 둘(비었다/가득 찼다)이라 cond 도 둘이다. 하나로 합치면 broadcast 로 엉뚱한 쪽까지
 * 깨워야 해서 불필요한 깨어남(thundering herd)이 생긴다.
 * 메인 스레드는 try_submit 을 쓴다: accept 루프는 절대 기다리면 안 되기 때문.
 *
 * 큐에는 int(보통 클라이언트 fd)만 담는다. 풀은 fd 를 닫지 않는다 — 소유권은 작업 함수에 있다.
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>

typedef struct threadpool threadpool_t;
typedef void (*threadpool_fn)(int item, void *arg);

/* 실패 시 NULL. nworkers, qcap 은 1 이상. */
threadpool_t *threadpool_create(size_t nworkers, size_t qcap, threadpool_fn fn, void *arg);

/* 큐가 가득 찼거나 종료 중이면 즉시 -1 (호출자가 503 처리). 성공 0. */
int threadpool_try_submit(threadpool_t *tp, int item);

/* 자리가 날 때까지 not_full 에서 기다린다. 종료 중이면 -1. */
int threadpool_submit_wait(threadpool_t *tp, int item);

/*
 * 새 작업을 더 받지 않고, 큐에 이미 들어온 작업은 모두 처리한 뒤 워커를 join 하고 해제한다.
 * (받아 둔 연결을 버리지 않는 graceful shutdown)
 */
void threadpool_destroy(threadpool_t *tp);

#endif /* THREADPOOL_H */
