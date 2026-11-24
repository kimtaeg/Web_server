/*
 * cache.c — LRU 캐시. 자료구조·크기 제한·동기화 설계는 cache.h 참고.
 */
#include "cache.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_DEFAULT_BUCKETS 4096

typedef struct entry {
    struct entry *hnext; /* 같은 버킷의 다음 항목 */
    struct entry *prev;  /* LRU: 더 최근 쪽 */
    struct entry *next;  /* LRU: 더 오래된 쪽 */
    uint64_t hash;
    size_t klen, vlen, aux, charge;
    long long expires_ms;
    char *key; /* 할당 한 번: [entry][key][val] */
    char *val;
} entry_t;

struct cache {
    pthread_rwlock_t lock;
    /* --- lock 으로 보호 --- */
    entry_t **buckets;
    size_t nb;
    entry_t *head, *tail;
    size_t bytes;
    size_t entries;
    /* --- 생성 후 불변 --- */
    size_t max_bytes, max_object;
    /* --- 원자 통계 (read lock 아래에서도 갱신) --- */
    _Atomic unsigned long long hits, misses, stores, evictions, rejected;
};

/* FNV-1a 64: 짧은 문자열에 빠르고 분포가 괜찮다. 보안용 해시가 아님 (키는 URL, 공격자 통제 가능 →
 * 버킷 쏠림 공격에 대한 방어는 없다. 상한이 있는 캐시라 최악도 체인 길이 = 항목 수). */
static uint64_t fnv1a(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

cache_t *cache_create(size_t max_bytes, size_t max_object, size_t nbuckets)
{
    cache_t *c = calloc(1, sizeof *c);

    if (c == NULL)
        return NULL;
    c->nb = nbuckets ? nbuckets : CACHE_DEFAULT_BUCKETS;
    c->buckets = calloc(c->nb, sizeof *c->buckets);
    if (c->buckets == NULL || pthread_rwlock_init(&c->lock, NULL) != 0) {
        free(c->buckets);
        free(c);
        return NULL;
    }
    c->max_bytes = max_bytes;
    c->max_object = max_object;
    return c;
}

void cache_destroy(cache_t *c)
{
    entry_t *e;

    if (c == NULL)
        return;
    for (e = c->head; e != NULL;) {
        entry_t *n = e->next;

        free(e);
        e = n;
    }
    pthread_rwlock_destroy(&c->lock);
    free(c->buckets);
    free(c);
}

/* ---- 아래 static 함수들은 모두 lock 을 잡은 상태에서 호출 ---- */

static entry_t *find_locked(cache_t *c, uint64_t h, const char *key, size_t klen)
{
    entry_t *e;

    for (e = c->buckets[h % c->nb]; e != NULL; e = e->hnext)
        if (e->hash == h && e->klen == klen && memcmp(e->key, key, klen) == 0)
            return e;
    return NULL;
}

static void list_unlink(cache_t *c, entry_t *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        c->head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void list_push_front(cache_t *c, entry_t *e)
{
    e->prev = NULL;
    e->next = c->head;
    if (c->head)
        c->head->prev = e;
    c->head = e;
    if (c->tail == NULL)
        c->tail = e;
}

/* 해시와 리스트에서 떼고 크기를 뺀다. free 는 호출자가 락 밖에서. */
static void remove_locked(cache_t *c, entry_t *e)
{
    entry_t **pp = &c->buckets[e->hash % c->nb];

    while (*pp != e)
        pp = &(*pp)->hnext;
    *pp = e->hnext;
    e->hnext = NULL;
    list_unlink(c, e);
    c->bytes -= e->charge;
    c->entries--;
}

/* ---- 공개 API ---- */

int cache_get(cache_t *c, const char *key, size_t klen, long long now_ms, buf_t *out, size_t *aux)
{
    uint64_t h = fnv1a(key, klen);
    entry_t *e;
    int rc;
    int expired = 0;

    /* 1) read lock: 찾고 복사 (여러 스레드 동시 가능) */
    pthread_rwlock_rdlock(&c->lock);
    e = find_locked(c, h, key, klen);
    if (e == NULL) {
        rc = CACHE_MISS;
    } else if (e->expires_ms <= now_ms) {
        rc = CACHE_MISS;
        expired = 1;
    } else if (buf_append(out, e->val, e->vlen) != BUF_OK) {
        rc = CACHE_ERR;
    } else {
        if (aux)
            *aux = e->aux;
        rc = CACHE_HIT;
    }
    pthread_rwlock_unlock(&c->lock);
    /* 여기서부터 e 는 다른 스레드가 해제했을 수 있다 → 다시 쓰지 않는다 */

    if (rc == CACHE_HIT || expired) {
        entry_t *victim = NULL;

        /* 2) write lock: 키로 다시 찾아서 맨 앞으로 (또는 만료 항목 제거) */
        pthread_rwlock_wrlock(&c->lock);
        e = find_locked(c, h, key, klen);
        if (e != NULL) {
            if (e->expires_ms <= now_ms) {
                remove_locked(c, e);
                victim = e;
            } else if (rc == CACHE_HIT) {
                list_unlink(c, e);
                list_push_front(c, e);
            }
        }
        pthread_rwlock_unlock(&c->lock);
        free(victim);
    }
    if (rc == CACHE_HIT)
        atomic_fetch_add(&c->hits, 1);
    else if (rc == CACHE_MISS)
        atomic_fetch_add(&c->misses, 1);
    return rc;
}

int cache_put(cache_t *c, const char *key, size_t klen, const void *val, size_t vlen, size_t aux,
              long long expires_ms)
{
    size_t charge = sizeof(entry_t) + klen + vlen;
    entry_t *e;
    entry_t *freelist = NULL;

    if (vlen > c->max_object || charge > c->max_bytes || klen > SIZE_MAX / 2) {
        atomic_fetch_add(&c->rejected, 1);
        return CACHE_TOO_BIG;
    }
    /* 할당과 복사는 락 밖에서 */
    e = malloc(charge);
    if (e == NULL)
        return CACHE_ERR;
    memset(e, 0, sizeof *e);
    e->key = (char *)(e + 1);
    e->val = e->key + klen;
    memcpy(e->key, key, klen);
    if (vlen)
        memcpy(e->val, val, vlen);
    e->hash = fnv1a(key, klen);
    e->klen = klen;
    e->vlen = vlen;
    e->aux = aux;
    e->charge = charge;
    e->expires_ms = expires_ms;

    pthread_rwlock_wrlock(&c->lock);
    {
        entry_t *old = find_locked(c, e->hash, key, klen);
        entry_t **bucket = &c->buckets[e->hash % c->nb];

        if (old != NULL) { /* 같은 키 교체 */
            remove_locked(c, old);
            old->hnext = freelist;
            freelist = old;
        }
        e->hnext = *bucket;
        *bucket = e;
        list_push_front(c, e);
        c->bytes += charge;
        c->entries++;
        /* 전체 상한: 가장 오래된 것부터 (방금 넣은 head 는 charge <= max_bytes 라 남는다) */
        while (c->bytes > c->max_bytes && c->tail != NULL && c->tail != e) {
            entry_t *t = c->tail;

            remove_locked(c, t);
            t->hnext = freelist;
            freelist = t;
            atomic_fetch_add(&c->evictions, 1);
        }
    }
    pthread_rwlock_unlock(&c->lock);
    atomic_fetch_add(&c->stores, 1);

    while (freelist != NULL) { /* free 도 락 밖에서 */
        entry_t *n = freelist->hnext;

        free(freelist);
        freelist = n;
    }
    return 0;
}

size_t cache_entry_overhead(void)
{
    return sizeof(entry_t);
}

void cache_stats(cache_t *c, cache_stats_t *out)
{
    out->hits = atomic_load(&c->hits);
    out->misses = atomic_load(&c->misses);
    out->stores = atomic_load(&c->stores);
    out->evictions = atomic_load(&c->evictions);
    out->rejected = atomic_load(&c->rejected);
    pthread_rwlock_rdlock(&c->lock);
    out->bytes = c->bytes;
    out->entries = c->entries;
    pthread_rwlock_unlock(&c->lock);
}

int cache_self_check(cache_t *c)
{
    entry_t *e;
    size_t n = 0, bytes = 0, inbuckets = 0, i;
    int rc = 0;

    pthread_rwlock_rdlock(&c->lock);
    for (e = c->head; e != NULL; e = e->next) {
        if ((e->prev == NULL) != (e == c->head) || (e->next == NULL) != (e == c->tail))
            rc = -1; /* 양 끝 포인터 불일치 */
        if (e->next && e->next->prev != e)
            rc = -1; /* 이중 연결 깨짐 */
        if (find_locked(c, e->hash, e->key, e->klen) != e)
            rc = -1; /* 리스트엔 있는데 해시에서 안 찾아짐 */
        n++;
        bytes += e->charge;
    }
    for (i = 0; i < c->nb; i++)
        for (e = c->buckets[i]; e != NULL; e = e->hnext)
            inbuckets++;
    if (n != c->entries || inbuckets != c->entries || bytes != c->bytes || c->bytes > c->max_bytes)
        rc = -1;
    pthread_rwlock_unlock(&c->lock);
    return rc;
}
