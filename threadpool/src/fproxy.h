/*
 * fproxy.h — 포워드 프록시 (+ LRU 캐시).
 *
 *   요청: GET http://host[:port]/path HTTP/1.1   (absolute-form, `curl -x proxy URL` 이 보내는 형태)
 *   → 대상 host:port 를 getaddrinfo 로 해석 → 주소를 순서대로 연결 시도
 *   → 업스트림에는 origin-form("/path?query")으로 재작성해 보냄 (upstream.c 재사용)
 *   → 응답을 클라이언트로 중계하면서 사본을 떠서 저장 조건이 맞으면 캐시에 넣는다.
 *
 * https://... absolute-form 과 CONNECT(터널)는 지원하지 않는다 → 501.
 *
 * [캐시 저장 조건] (모두 만족)
 *   GET 요청, Authorization 없음 (RFC 9111 3.5: 공유 캐시는 인증된 응답을 남에게 주면 안 된다)
 *   200 응답, Content-Length 있음, 끝까지 받음, 크기 ≤ 객체 상한
 *   Cache-Control 에 no-store / private / no-cache 없음, max-age=0 아님
 *   Set-Cookie 없음 (한 사용자의 세션 쿠키가 다른 사용자에게 나가는 사고 방지)
 * [TTL] s-maxage > max-age > 기본 TTL(cache_default_ttl_secs) 순.
 * [응답 헤더] X-Cache: HIT 또는 MISS. HEAD 는 캐시된 GET 의 헤더로 HIT 처리한다.
 *
 * [보안] 포워드 프록시는 아무나 내부망으로 요청을 보낼 수 있는 통로가 된다(open proxy, SSRF).
 *        그래서 기본값은 꺼짐(forward_proxy off)이고, 켜려면 설정에서 명시해야 한다.
 *        Via 에 자기 인스턴스 표식이 있으면 루프로 보고 508.
 */
#ifndef FPROXY_H
#define FPROXY_H

#include <stddef.h>

#include "cache.h"
#include "conn.h"

typedef struct {
    cache_t *cache; /* NULL = 캐시 없이 중계만 */
    int connect_timeout_ms;
    int read_timeout_ms;
    long long default_ttl_ms;
} fproxy_ctx_t;

/* ---- 순수 함수 (단위 테스트 대상) ---- */

/*
 * 캐시 키: "http://" + 소문자 host + (":port", 80 이면 생략) + path + ("?" + query).
 * 같은 자원을 가리키는 표기 차이(HOST 대소문자, :80 유무)를 하나로 모은다. 경로는 대소문자 구분.
 * 반환: 길이, 실패(버퍼 부족/잘못된 authority) -1.
 */
int fproxy_cache_key(const http_request_t *req, char *out, size_t outsz);

/* authority "host[:port]" / "[v6]:port" → host 문자열, port(기본 80). 성공 0. */
int fproxy_split_authority(const char *a, size_t n, char *host, size_t hostsz, int *port);

/*
 * head(상태줄+헤더, 빈 줄 없음)가 저장 가능한 응답인지 판정하고 TTL 을 정한다.
 * 저장 가능하면 1 과 *ttl_ms, 아니면 0.
 */
int fproxy_cacheable(const http_request_t *req, const char *head, size_t head_len,
                     long long default_ttl_ms, long long *ttl_ms);

/* conn_handler_fn. ctx 는 fproxy_ctx_t*. */
int fproxy_handle(void *ctx, conn_t *cl, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res);

#endif /* FPROXY_H */
