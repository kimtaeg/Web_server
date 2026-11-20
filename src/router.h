/*
 * router.h — 요청 경로 → 핸들러 선택.
 *
 * 경로 접두사 규칙 목록에서 "가장 길게 일치하는" 규칙을 고르고, 없으면 기본(fallback) 핸들러.
 * 접두사는 경로 구성요소 경계에서만 일치한다: "/cgi-bin" 은 "/cgi-bin", "/cgi-bin/x" 와 일치하지만
 * "/cgi-binx" 와는 일치하지 않는다.
 *
 * 매칭은 디코딩 전 원본 경로로 한다. 그래서 "/cgi%2dbin/x" 는 CGI 규칙에 걸리지 않고
 * static 으로 간다. 이것이 안전하려면 CGI 디렉터리가 문서 루트 밖에 있어야 한다 (기본 구성).
 *
 * 동기화: 시작 시 router_add 로 구성한 뒤에는 읽기만 하므로 락이 필요 없다.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>

#include "conn.h"

#define ROUTER_MAX_ROUTES 16
#define ROUTER_MAX_PREFIX 256

typedef struct {
    char prefix[ROUTER_MAX_PREFIX]; /* 끝의 '/' 는 제거해서 저장 ("/" 는 "") */
    size_t len;
    conn_handler_fn fn;
    void *ctx;
} route_t;

typedef struct {
    route_t routes[ROUTER_MAX_ROUTES];
    size_t n;
    conn_handler_fn fallback;
    void *fallback_ctx;
} router_t;

void router_init(router_t *r, conn_handler_fn fallback, void *fallback_ctx);

/* prefix 는 '/' 로 시작해야 한다. 가득 찼거나 형식이 틀리면 -1. */
int router_add(router_t *r, const char *prefix, conn_handler_fn fn, void *ctx);

/* 순수 함수: 가장 긴 일치 규칙. 없으면 NULL (→ fallback). */
const route_t *router_match(const router_t *r, const char *path, size_t path_len);

/* conn_handler_fn. ctx 는 router_t*. */
int router_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res);

#endif /* ROUTER_H */
