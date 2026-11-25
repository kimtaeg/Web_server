/*
 * connio.h — 클라이언트 연결 하나의 읽기/쓰기. 평문(HTTP)과 TLS(HTTPS)를 같은 함수로 다룬다.
 *
 * [왜 래퍼인가]
 * 핸들러(static, cgi, rproxy ...)는 "응답 바이트를 클라이언트에 보낸다"만 알면 된다.
 * 그 아래가 send() 인지 SSL_write() 인지는 몰라도 되도록, 클라이언트 쪽 I/O 는 전부 여기를 거친다.
 *   ssl == NULL → netio 의 send_all / recv_some 그대로 (기존 HTTP 동작과 바이트 단위로 같다)
 *   ssl != NULL → SSL_write / SSL_read (OpenSSL 이 암호화·복호화)
 * 업스트림(서버 → 백엔드) 소켓은 항상 평문이라 여기를 거치지 않고 netio 를 직접 쓴다.
 *
 * 헤더에서 OpenSSL 을 include 하지 않는다: SSL 은 불완전 타입(struct ssl_st)으로만 들고 있어,
 * 핸들러 모듈들은 OpenSSL 헤더 없이도 컴파일된다.
 *
 * 동기화: conn_t 하나는 워커 하나만 쓴다 (연결 = 워커 하나). 락 없음.
 */
#ifndef CONNIO_H
#define CONNIO_H

#include <stddef.h>
#include <sys/types.h>

struct ssl_st;

typedef struct {
    int fd;
    struct ssl_st *ssl; /* NULL = 평문 */
    /*
     * TLS 연결에서 치명적 오류(SSL_ERROR_SYSCALL / SSL_ERROR_SSL)가 났는지.
     * 이 상태에서 SSL_shutdown 을 부르면 안 된다 (OpenSSL 문서). tls_close 가 본다.
     */
    int tls_broken;
} conn_t;

/* 평문 연결로 초기화. */
void conn_init_plain(conn_t *c, int fd);

/*
 * n 바이트를 전부 보낸다. 성공 n, 실패 -1.
 * TLS: SSL_write 를 부분 쓰기가 끝날 때까지 반복한다. 소켓은 blocking + SO_SNDTIMEO 라서
 *      응답을 읽지 않는 클라이언트는 SSL_ERROR_WANT_WRITE(타임아웃)로 끝난다 → -1.
 */
ssize_t conn_write(conn_t *c, const void *p, size_t n);

/*
 * 한 번 받는다. 반환은 netio 의 recv_some 과 같다:
 *   >0 받은 바이트 / 0 EOF / -1 오류 / NETIO_TIMEOUT.
 * TLS: OpenSSL 안에 이미 복호화해 둔 데이터가 있으면(SSL_pending) poll 없이 바로 읽는다.
 *      그 데이터는 커널 소켓 버퍼에 없어서 poll 은 "읽을 것 없음"이라고 답하기 때문이다.
 */
ssize_t conn_read(conn_t *c, void *p, size_t n, int timeout_ms);

#endif /* CONNIO_H */
