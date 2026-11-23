#include "cache.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"

#define CACHE_BUCKETS 1024

typedef struct entry {
    char *key;
    char *data;
    size_t len;
    long long stored_at;
    long long expires_at;
    struct entry *hnext;            /* hash chain */
    struct entry *prev, *next;      /* LRU list: head = most recent */
} entry_t;

struct cache {
    pthread_mutex_t lock;
    entry_t *buckets[CACHE_BUCKETS];
    entry_t *head, *tail;
    size_t bytes, entries, max_bytes, max_object;
    unsigned long long hits, misses, evictions;
};

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

cache_t *cache_create(size_t max_bytes, size_t max_object) {
    cache_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    pthread_mutex_init(&c->lock, NULL);
    c->max_bytes = max_bytes;
    c->max_object = max_object < max_bytes ? max_object : max_bytes;
    return c;
}

static void lru_unlink(cache_t *c, entry_t *e) {
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(cache_t *c, entry_t *e) {
    e->prev = NULL;
    e->next = c->head;
    if (c->head) c->head->prev = e;
    c->head = e;
    if (!c->tail) c->tail = e;
}

static void entry_remove(cache_t *c, entry_t *e) {
    entry_t **pp = &c->buckets[fnv1a(e->key) % CACHE_BUCKETS];
    while (*pp && *pp != e)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = e->hnext;
    lru_unlink(c, e);
    c->bytes -= e->len;
    c->entries--;
    free(e->key);
    free(e->data);
    free(e);
}

static entry_t *lookup(cache_t *c, const char *key) {
    for (entry_t *e = c->buckets[fnv1a(key) % CACHE_BUCKETS]; e; e = e->hnext)
        if (strcmp(e->key, key) == 0)
            return e;
    return NULL;
}

void cache_destroy(cache_t *c) {
    if (!c)
        return;
    while (c->head)
        entry_remove(c, c->head);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

int cache_get(cache_t *c, const char *key, long long now, char **data, size_t *len,
              long long *age) {
    int hit = 0;
    pthread_mutex_lock(&c->lock);
    entry_t *e = lookup(c, key);
    if (e && e->expires_at <= now) {
        entry_remove(c, e);
        e = NULL;
    }
    if (e && (*data = malloc(e->len)) != NULL) {
        memcpy(*data, e->data, e->len);
        *len = e->len;
        *age = now - e->stored_at;
        lru_unlink(c, e);
        lru_push_front(c, e);
        c->hits++;
        hit = 1;
    } else {
        c->misses++;
    }
    pthread_mutex_unlock(&c->lock);
    return hit;
}

int cache_put(cache_t *c, const char *key, const char *data, size_t len, long long now,
              int ttl) {
    if (ttl <= 0 || len > c->max_object)
        return -1;
    entry_t *e = calloc(1, sizeof(*e));
    if (!e)
        return -1;
    e->key = strdup(key);
    e->data = malloc(len ? len : 1);
    if (!e->key || !e->data) {
        free(e->key);
        free(e->data);
        free(e);
        return -1;
    }
    memcpy(e->data, data, len);
    e->len = len;
    e->stored_at = now;
    e->expires_at = now + ttl;

    pthread_mutex_lock(&c->lock);
    entry_t *old = lookup(c, key);
    if (old)
        entry_remove(c, old);
    while (c->tail && c->bytes + len > c->max_bytes) {
        entry_remove(c, c->tail);
        c->evictions++;
    }
    uint32_t b = fnv1a(key) % CACHE_BUCKETS;
    e->hnext = c->buckets[b];
    c->buckets[b] = e;
    lru_push_front(c, e);
    c->bytes += len;
    c->entries++;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

void cache_stats(cache_t *c, cache_stats_t *out) {
    pthread_mutex_lock(&c->lock);
    out->entries = c->entries;
    out->bytes = c->bytes;
    out->hits = c->hits;
    out->misses = c->misses;
    out->evictions = c->evictions;
    pthread_mutex_unlock(&c->lock);
}

/* strcasestr is a GNU/BSD extension; keep to POSIX. */
static const char *ci_find(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return hay;
    return NULL;
}

int cache_ttl_from_cache_control(const char *value) {
    if (!value)
        return -1;
    if (http_header_has_token(value, "no-store") || http_header_has_token(value, "private") ||
        http_header_has_token(value, "no-cache"))
        return 0;
    const char *p = value;
    while ((p = ci_find(p, "max-age")) != NULL) {
        int boundary = (p == value || p[-1] == ' ' || p[-1] == ',');
        p += 7;
        if (!boundary)
            continue;
        while (*p == ' ')
            p++;
        if (*p != '=')
            continue;
        p++;
        while (*p == ' ')
            p++;
        if (*p == '"')
            p++;
        if (!isdigit((unsigned char)*p))
            return 0;
        long v = strtol(p, NULL, 10);
        return v > 86400 * 365 ? 86400 * 365 : (int)v;
    }
    return -1;
}
