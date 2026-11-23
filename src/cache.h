#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

/* Thread-safe LRU cache of whole HTTP responses with per-entry TTL. */
typedef struct cache cache_t;

typedef struct {
    size_t entries;
    size_t bytes;
    unsigned long long hits;
    unsigned long long misses;
    unsigned long long evictions;
} cache_stats_t;

cache_t *cache_create(size_t max_bytes, size_t max_object);
void cache_destroy(cache_t *c);

/* On hit returns 1 and a malloc'd copy the caller frees (so no lock is held while
 * sending). *age is seconds since the entry was stored. */
int cache_get(cache_t *c, const char *key, long long now, char **data, size_t *len,
              long long *age);
/* Returns 0 if stored, -1 if rejected (too large, ttl <= 0, no memory). */
int cache_put(cache_t *c, const char *key, const char *data, size_t len, long long now,
              int ttl);
void cache_stats(cache_t *c, cache_stats_t *out);

/* Freshness lifetime from Cache-Control; 0 = do not store, -1 = no directive. */
int cache_ttl_from_cache_control(const char *value);

#endif
