/*
 * tls.h — OpenSSL 을 이용한 서버 쪽 TLS (HTTPS). 프로토콜·암호는 직접 구현하지 않는다.
 *
 *   시작 시 (main)   : tls_ctx_create  → SSL_CTX 하나 (인증서·개인키·TLS 1.2 이상)
 *   연결마다 (워커)  : tls_accept      → SSL 객체 + 핸드셰이크
 *                      conn_read/write → SSL_read/SSL_write (connio.h)
 *                      tls_close       → close_notify + SSL_free
 *   종료 시 (main)   : tls_ctx_free    → 워커가 전부 끝난 뒤
 *
 * [동기화]
 * - SSL_CTX: main 이 워커를 만들기 전에 한 번 설정하고 이후 읽기만 한다. 여러 워커가 같은 CTX 로
 *   SSL_new 를 동시에 불러도 된다 (OpenSSL 1.1.0+ 는 CTX 내부 공유 상태(세션 캐시, 참조 카운트)를
 *   스스로 락으로 보호한다).
 * - SSL: 연결 하나에 하나, 그 연결을 맡은 워커만 쓴다. 락 없음.
 * - OpenSSL 오류 큐는 스레드별(thread-local)이다. 다른 워커의 오류가 섞이지 않는다.
 *
 * [blocking 소켓 + SSL_MODE_AUTO_RETRY]
 * 데이터 단계는 기존 구조대로 blocking 소켓을 쓴다. AUTO_RETRY 를 켜면 TLS 1.3 의 세션 티켓 같은
 * 비데이터 레코드를 처리한 뒤 SSL_read 가 WANT_READ 로 돌아오지 않고 다음 데이터를 기다린다.
 * 그래서 데이터 단계의 WANT_READ/WANT_WRITE 는 "소켓 타임아웃(SO_RCVTIMEO/SO_SNDTIMEO)" 뜻이 된다.
 *
 * 핸드셰이크만은 잠시 non-blocking 으로 돌려 poll + 마감 시각으로 기다린다 (tls_accept).
 * blocking 으로 하면 아무것도 보내지 않는 클라이언트가 워커를 붙잡는 시간을 전체 마감으로
 * 묶을 수 없기 때문이다. 여기서 SSL_ERROR_WANT_READ / WANT_WRITE 를 직접 처리한다.
 */
#ifndef TLS_H
#define TLS_H

#include <signal.h>

#include "connio.h"
#include "log.h"

struct ssl_ctx_st;

/*
 * OpenSSL 초기화 + SSL_CTX 생성. 실패하면 원인을 로그로 남기고 NULL.
 *   TLS_server_method, 최소 TLS 1.2, 인증서 체인(PEM) 등록, 개인키(PEM) 등록,
 *   SSL_CTX_check_private_key 로 짝 검증, SSL_MODE_AUTO_RETRY.
 */
struct ssl_ctx_st *tls_ctx_create(const char *cert_path, const char *key_path);

/* NULL 이면 아무것도 하지 않는다. */
void tls_ctx_free(struct ssl_ctx_st *ctx);

/* tls_accept 결과 */
#define TLS_OK 0
#define TLS_FAIL (-1)      /* 핸드셰이크 실패 (상대가 TLS 가 아님, 버전·암호 불일치, 끊김 ...) */
#define TLS_TIMEOUT (-2)   /* 마감까지 끝나지 않음 또는 서버 종료 중 */
#define TLS_PLAIN_HTTP (-3) /* HTTPS 포트에 평문 HTTP 요청이 옴 (호출자가 평문 400 을 보낼 수 있다) */

/*
 * c->fd(평문으로 init 된 상태)에 SSL 객체를 만들어 붙이고 핸드셰이크를 한다.
 *   SSL_new → SSL_set_fd → SSL_accept (WANT_READ/WANT_WRITE 면 poll 후 재시도)
 * 성공하면 c->ssl 이 설정되고 소켓은 다시 blocking. 실패하면 SSL 을 해제하고 c->ssl == NULL
 * (소켓은 호출자가 닫는다). stop 이 설정되면 TLS_TIMEOUT.
 */
int tls_accept(struct ssl_ctx_st *ctx, conn_t *c, int timeout_ms, volatile sig_atomic_t *stop);

/*
 * TLS 를 정리한다: (오류 상태가 아니면) SSL_shutdown 으로 close_notify 를 한 번 보내고 SSL_free.
 * 상대의 close_notify 는 기다리지 않는다 (HTTP 는 Content-Length 로 경계를 알리므로 충분하다).
 * 소켓 자체는 닫지 않는다. c->ssl 이 NULL 이면 아무것도 하지 않는다.
 */
void tls_close(conn_t *c);

/* tls_io_fail 결과: 실패한 SSL_read/SSL_write 를 어떻게 다룰지 */
#define TLS_IO_EOF 0       /* 상대가 정상 종료(close_notify) 또는 close_notify 없이 끊음 */
#define TLS_IO_TIMEOUT 1   /* WANT_READ/WANT_WRITE: blocking 소켓에서는 SO_RCVTIMEO/SO_SNDTIMEO 만료 */
#define TLS_IO_ERROR 2     /* 그 밖의 오류 */

/*
 * 실패한 SSL_* 호출(ret <= 0)을 분류하고 로그로 남긴다. 호출 직후(errno 가 바뀌기 전) 불러야 한다.
 *   SSL_get_error 결과 이름 + errno + 스레드의 OpenSSL 오류 큐(ERR_print_errors_cb → log_msg).
 *   상대가 끊은 경우처럼 흔한 일은 DEBUG, 그 밖은 WARN.
 * SSL_ERROR_SYSCALL / SSL_ERROR_SSL 이면 c->tls_broken = 1 (이후 SSL_shutdown 금지).
 */
int tls_io_fail(conn_t *c, const char *what, int ret);

/* OpenSSL 오류 큐를 로그로 옮기고 비운다 (SSL 객체가 없는 단계, 예: CTX 설정 실패). */
void tls_log_queue(log_level_t lvl);

#endif /* TLS_H */
