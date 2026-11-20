/*
 * cache.h — 문자열 키 → 바이트 값 LRU 캐시 (해시맵 + 이중 연결 리스트), 스레드 안전.
 *
 * [자료구조]
 *   해시맵(체이닝, FNV-1a)  : 키 → 항목, O(1) 조회
 *   이중 연결 리스트         : 최근 사용 순서. head = 가장 최근, tail = 다음 축출 대상
 *   → 조회/삽입/축출/맨 앞으로 이동이 모두 O(1).
 *
 * [크기 제한]
 *   항목 비용(charge) = 항목 구조체 + 키 + 값.
 *   값이 max_object 를 넘으면 저장하지 않는다 (큰 파일 하나가 캐시 전체를 밀어내는 것 방지).
 *   전체가 max_bytes 를 넘으면 tail 부터 연쇄 축출한다.
 *
 * [동기화: pthread_rwlock 하나]
 *   LRU 는 "읽기(get)도 순서를 바꾸는 쓰기"라서 read lock 만으로는 정확한 LRU 를 유지할 수 없다.
 *   그래서 get 을 두 단계로 나눈다:
 *     1) read lock : 찾아서 값 복사 (덩치 큰 memcpy 를 여러 스레드가 동시에 할 수 있다)
 *     2) write lock: 키로 "다시" 찾아서 맨 앞으로 이동 (포인터 몇 개라 아주 짧다)
 *   1 과 2 사이에 다른 스레드가 그 항목을 축출/교체했을 수 있으므로, 1 에서 얻은 포인터를 2 에서
 *   쓰지 않고 키로 다시 찾는다(해제된 메모리 접근 방지). 없어졌으면 옮기지 않는다.
 *   put 은 할당과 복사를 락 밖에서 하고, write lock 안에서는 연결만 바꾼다. 축출된 항목의 free 도
 *   락 밖에서 한다 → write lock 을 잡는 시간이 최소.
 *   hits/misses 같은 통계는 read lock 아래에서도 동시에 증가하므로 원자 변수다.
 */
#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

#include "buf.h"

#define CACHE_HIT 0
#define CACHE_MISS 1
#define CACHE_ERR (-1)     /* 메모리 부족, out 버퍼 상한 */
#define CACHE_TOO_BIG (-2) /* put: 객체/전체 상한 초과로 저장 안 함 */

typedef struct cache cache_t;

typedef struct {
    unsigned long long hits, misses, stores, evictions, rejected;
    size_t bytes;   /* 현재 사용량 (charge 합) */
    size_t entries;
} cache_stats_t;

/* nbuckets 0 이면 기본값(4096). 실패 NULL. */
cache_t *cache_create(size_t max_bytes, size_t max_object, size_t nbuckets);
void cache_destroy(cache_t *c);

/*
 * 키로 조회. HIT 이면 값을 out 에 덧붙이고 *aux 에 저장 시 받은 부가 값. now_ms 가 expires_ms 이상이면
 * 만료로 보고 MISS (그 항목은 지운다). 반환 CACHE_HIT / CACHE_MISS / CACHE_ERR.
 */
int cache_get(cache_t *c, const char *key, size_t klen, long long now_ms, buf_t *out, size_t *aux);

/* 저장(같은 키면 교체). expires_ms 이후로는 MISS. 반환 0 / CACHE_TOO_BIG / CACHE_ERR. */
int cache_put(cache_t *c, const char *key, size_t klen, const void *val, size_t vlen, size_t aux,
              long long expires_ms);

void cache_stats(cache_t *c, cache_stats_t *out);

/* 항목 하나의 고정 비용(구조체 크기). charge = 이 값 + 키 길이 + 값 길이. 테스트·용량 계산용. */
size_t cache_entry_overhead(void);

/* 테스트용: 리스트·해시·크기 합계의 일관성 검사. 정상이면 0. */
int cache_self_check(cache_t *c);

#endif /* CACHE_H */
