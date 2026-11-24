/*
 * conn.h — 연결 하나의 keep-alive 루프 (워커 스레드에서 실행).
 *
 *   loop: recv(poll 타임아웃) → http_parse → handler → 응답 → (keep-alive 면) 다음 요청
 *
 * 핸들러는 함수 포인터로 받는다. 2단계에서는 static_handle, 3단계부터는 router 가 들어온다.
 * conn 은 "요청 경계와 연결 수명"만 책임지고, 응답 내용은 모른다.
 */
#ifndef CONN_H
#define CONN_H

#include <signal.h>
#include <stddef.h>

#include "http_parser.h"

typedef struct {
    int status;   /* 접근 로그용 응답 상태 */
    size_t bytes; /* 보낸 바이트 수 */
    int close;    /* 핸들러가 연결을 끊어야 한다고 판단 (예: 약속한 Content-Length 를 못 채움) */
} handler_result_t;

/* 연결 단위 정보. 요청 단위가 아니라서 http_request_t 와 분리했다. */
typedef struct {
    const char *ip; /* 상대 IP 문자열 ("127.0.0.1", "::1", 알 수 없으면 "-") */
} conn_info_t;

/*
 * 응답을 fd 로 직접 보낸다. keep_alive 는 conn 이 결정한 값이며, 핸들러는 그대로
 * Connection 헤더에 반영해야 한다. 반환 0 = 정상, -1 = I/O 실패(연결 종료).
 */
typedef int (*conn_handler_fn)(void *ctx, int fd, const http_request_t *req,
                               const conn_info_t *ci, int keep_alive, handler_result_t *res);

typedef struct {
    http_limits_t limits;
    int idle_timeout_ms;   /* recv 한 번을 기다리는 최대 시간 (바이트가 오면 다시 센다) */
    /*
     * 요청의 첫 바이트부터 헤더 끝(빈 줄)까지 허용하는 총 시간. 0 이면 끔.
     * idle_timeout 만으로는 slowloris(헤더를 idle 보다 짧은 간격으로 1바이트씩)를 막지 못한다:
     * 바이트가 올 때마다 idle 타이머가 다시 시작되기 때문. 이 마감은 다시 시작되지 않는다. 초과 → 408.
     */
    int header_timeout_ms;
    int send_timeout_ms; /* SO_SNDTIMEO: 응답을 읽지 않는 클라이언트 방어 */
    int max_requests;    /* 연결당 최대 요청 수. 마지막 응답에 Connection: close */
    conn_handler_fn handler;
    void *handler_ctx;
    volatile sig_atomic_t *stop; /* NULL 가능. 설정되면 진행 중 요청까지만 처리하고 닫는다 */
} conn_config_t;

/* fd 의 소유권을 가져가 반드시 닫는다. */
void conn_handle(int fd, const conn_config_t *cfg);

/* threadpool_fn 어댑터: arg 는 const conn_config_t*. */
void conn_worker(int fd, void *arg);

#endif /* CONN_H */
