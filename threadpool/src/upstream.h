/*
 * upstream.h — 업스트림(백엔드) 연결, 요청 재작성, 응답 중계. rproxy 와 fproxy 가 공유한다.
 *
 * [연결] non-blocking connect + poll 로 연결 타임아웃. 연결 뒤에는 blocking 으로 되돌리고,
 *        읽기는 recv_some(poll) 로 읽기 타임아웃, 쓰기는 SO_SNDTIMEO.
 *
 * [요청 재작성]
 *   제거: hop-by-hop 헤더(RFC 9110 7.6.1) — Connection, Keep-Alive, Proxy-*, TE, Trailer,
 *         Transfer-Encoding, Upgrade — 와 Connection 헤더에 이름이 나열된 헤더.
 *         Expect 도 뺀다: 프록시는 본문을 이미 다 받은 상태다.
 *   추가: X-Forwarded-For(기존 값 뒤에 클라이언트 IP), X-Forwarded-Proto, Via,
 *         Connection: close — 업스트림 연결은 요청 하나로 끝낸다(커넥션 풀 없음).
 *         그래서 업스트림 EOF 가 곧 응답 끝이라는 단순한 규칙을 쓸 수 있다.
 *   유지: Host 와 경로 (접두사를 떼지 않는다).
 *
 * [응답 중계] 상태줄·헤더를 파싱해 hop-by-hop 을 지우고 클라이언트 쪽 Connection 으로 바꾼 뒤,
 *   본문은 고정 버퍼로 받는 즉시 흘려보낸다(스트리밍: 응답 크기와 무관하게 메모리 일정).
 *   - Content-Length 있음 : 정확히 그만큼 중계, 클라이언트 keep-alive 유지
 *   - 없음(EOF 구분)      : 업스트림 EOF 까지 중계하고 클라이언트 연결을 닫는다
 *   - chunked             : 바이트를 그대로 통과 + 응답 후 닫음. HTTP/1.0 클라이언트면 502
 *   - HEAD / 1xx / 204 / 304 : 본문 없음
 */
#ifndef UPSTREAM_H
#define UPSTREAM_H

#include <stddef.h>
#include <sys/socket.h>

#include "buf.h"
#include "connio.h"
#include "http_parser.h"

#define UPSTREAM_OK 0
#define UPSTREAM_ECONN (-1)    /* 연결 실패 (거부, 도달 불가) → 502 */
#define UPSTREAM_ETIMEOUT (-2) /* 연결/읽기 타임아웃, 클라이언트에 아직 아무것도 안 보냄 → 504 */
#define UPSTREAM_EBAD (-3)     /* 응답 형식 오류·헤더 전 EOF, 아직 아무것도 안 보냄 → 502 */
#define UPSTREAM_EPARTIAL (-4) /* 응답 일부를 보낸 뒤 업스트림 실패 → 클라이언트 연결 종료 */
#define UPSTREAM_ECLIENT (-5)  /* 클라이언트로 보내기 실패 → 연결 종료 */
/*
 * 프록시 자신의 자원 부족(임시 포트 고갈, fd 고갈 등)으로 연결 시도조차 못 함 → 503.
 * 백엔드 잘못이 아니므로 down 으로 표시하지 않고, 다른 백엔드로 재시도하지도 않는다(같은 이유로 실패).
 * 실측: 업스트림 커넥션 풀이 없어 요청마다 새 연결 → TIME_WAIT 가 임시 포트(macOS 16384개)를 다 쓰면
 * connect 가 EADDRNOTAVAIL. 이를 백엔드 장애로 처리하면 순간 포트 부족이 10초짜리 전체 502 가 된다.
 */
#define UPSTREAM_ELOCAL (-6)

#define UPSTREAM_MAX_HEAD (64 * 1024)
#define UPSTREAM_MAX_RESP_HEADERS 100
#define UPSTREAM_VIA_DEFAULT "1.1 cweb"

typedef struct {
    int status;
    size_t reason_off, reason_len;
    size_t head_len; /* 빈 줄까지 포함한 헤더 영역 길이 */
    int has_cl;
    size_t content_length;
    int chunked;
    size_t nh;
    size_t name_off[UPSTREAM_MAX_RESP_HEADERS], name_len[UPSTREAM_MAX_RESP_HEADERS];
    size_t val_off[UPSTREAM_MAX_RESP_HEADERS], val_len[UPSTREAM_MAX_RESP_HEADERS];
} upstream_resp_t;

/*
 * 캐시용 응답 사본. upstream_relay 가 클라이언트로 흘려보내면서 같은 바이트를 여기에도 쌓는다.
 *   head : 클라이언트에 보낸 상태줄+헤더 중 Connection 과 extra_headers 를 뺀 것 (빈 줄 없음)
 *   body : 본문. body.max 를 넘으면 더 쌓지 않고 overflow = 1
 *   complete : 응답을 Content-Length 만큼 정확히 다 받았고 overflow 가 없음 → 저장해도 되는 사본
 */
typedef struct {
    buf_t head;
    buf_t body;
    int status;
    int overflow;
    int complete;
} upstream_capture_t;

typedef struct {
    int status;            /* 백엔드 응답 상태 (로그용) */
    size_t bytes;          /* 클라이언트로 보낸 바이트 */
    int client_keep_alive; /* 중계 후 클라이언트 연결을 유지해도 되는가 */
} upstream_result_t;

/* ---- 순수 함수 (단위 테스트 대상) ---- */

int upstream_is_hop_by_hop(const char *name, size_t n);

/* connect/socket 의 errno 가 "우리 쪽 자원 부족"인가 (EADDRNOTAVAIL, EMFILE, ENFILE, ENOBUFS, EAGAIN) */
int upstream_is_local_error(int err);

/*
 * Via 표식. 인스턴스마다 다른 값("1.1 cweb-<id>")을 쓰면, 들어온 요청의 Via 에 자기 표식이 있을 때
 * "내가 보낸 요청이 나에게 돌아왔다" = 프록시 루프임을 알 수 있다 (→ 508).
 * 고정 문자열이면 cweb 앞에 cweb 을 둔 정상 체인까지 루프로 오판한다.
 * 동기화: main 이 워커를 만들기 전에 한 번만 설정, 이후 읽기 전용. 설정 안 하면 UPSTREAM_VIA_DEFAULT.
 */
void upstream_set_via(const char *via);
const char *upstream_via(void);
int upstream_request_has_our_via(const http_request_t *req);

/*
 * 업스트림으로 보낼 요청 전체(요청줄 + 재작성한 헤더 + 본문)를 out 에 만든다.
 * target: 요청줄에 쓸 대상(origin-form). host_fallback: 요청에 Host 가 없을 때(HTTP/1.0) 쓸 값.
 */
int upstream_build_request(buf_t *out, const http_request_t *req, const char *target,
                           size_t target_len, const char *host_fallback, const char *client_ip);

/* 응답 상태줄+헤더 파싱. 0 = 완료, 1 = 더 필요, -1 = 형식 오류. */
int upstream_parse_response_head(const char *p, size_t n, upstream_resp_t *r);

/* ---- I/O ---- */

/* 연결된 blocking fd(CLOEXEC, SIGPIPE 방어 적용) 또는 UPSTREAM_ECONN / ETIMEOUT / ELOCAL. */
int upstream_connect(const struct sockaddr *sa, socklen_t len, int timeout_ms);

/*
 * 요청은 이미 보냈다고 가정하고 ufd 의 응답을 클라이언트 cl 로 중계한다 (업스트림 쪽은 항상 평문). 반환 UPSTREAM_*.
 * extra_headers: 클라이언트 응답 헤더에 덧붙일 줄들 ("X-Cache: MISS\r\n"), NULL 가능.
 * cap: NULL 이 아니면 응답 사본을 쌓는다 (호출자가 head/body 를 buf_init 해 둘 것).
 */
int upstream_relay(int ufd, conn_t *cl, const http_request_t *req, int client_keep_alive,
                   int read_timeout_ms, const char *extra_headers, upstream_capture_t *cap,
                   upstream_result_t *res);

#endif /* UPSTREAM_H */
