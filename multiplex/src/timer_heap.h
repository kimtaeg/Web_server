/*
 * timer_heap.h — 마감 시각 최소 힙. 이벤트 루프의 연결별 타임아웃(유휴, 헤더 마감, 전송, 종료 대기)용.
 *
 * [왜 힙인가]
 * 이벤트 루프는 "다음 마감까지 몇 ms 기다리면 되나"(wait 타임아웃)와 "지금 만료된 것들"을 자주 묻는다.
 * - 연결 전체를 매 루프 훑기: O(n). 연결 1만 개면 루프마다 1만 번 검사.
 * - 최소 힙: 가장 이른 마감 확인 O(1), 추가/삭제/변경 O(log n).
 * 대안: 타이머 휠(O(1), 해상도 고정) — 연결 수가 아주 많을 때 유리하지만 구현이 길다.
 *       타임아웃 종류가 같으면 정렬 리스트(뒤에 붙이기 O(1))도 되지만, 여기는 종류가 섞인다.
 *
 * [위치 추적]
 * 노드가 힙 안에서 움직일 때마다 owner 가 준 *slot 에 현재 위치를 써 준다. 그래서 연결이 닫히거나
 * 마감이 바뀔 때 탐색 없이 바로 remove/update 할 수 있다. 힙에 없으면 *slot == TIMER_NONE.
 *
 * 동기화: 단일 스레드(이벤트 루프)만 쓴다. 락 없음.
 */
#ifndef TIMER_HEAP_H
#define TIMER_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define TIMER_NONE SIZE_MAX

typedef struct {
    long long deadline_ms;
    void *owner;
    size_t *slot; /* owner 쪽 위치 저장소 */
} timer_node_t;

typedef struct {
    timer_node_t *a;
    size_t n, cap;
} timer_heap_t;

void timer_heap_init(timer_heap_t *h);
void timer_heap_free(timer_heap_t *h);

/* 이미 힙에 있으면(*slot != TIMER_NONE) 마감만 바꾼다. 반환 0, 메모리 부족 -1. */
int timer_heap_set(timer_heap_t *h, long long deadline_ms, void *owner, size_t *slot);

/* 힙에 없으면 아무 일도 안 한다. 이후 *slot == TIMER_NONE. */
void timer_heap_remove(timer_heap_t *h, size_t *slot);

/* 가장 이른 마감. 비었으면 -1. owner 가 NULL 이 아니면 그 노드의 owner. */
long long timer_heap_peek(const timer_heap_t *h, void **owner);

size_t timer_heap_size(const timer_heap_t *h);

/* 테스트용: 힙 성질과 slot 일관성 검사. 정상 1. */
int timer_heap_check(const timer_heap_t *h);

#endif /* TIMER_HEAP_H */
