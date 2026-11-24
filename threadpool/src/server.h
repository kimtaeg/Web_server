/*
 * server.h — listen 소켓 생성과 accept 루프 (메인 스레드).
 *
 * 메인 스레드는 accept 와 큐 투입만 한다. 요청을 읽거나 처리하는 일은 전부 워커가 한다.
 * 큐가 가득 차면 그 자리에서 503 을 best-effort 로 보내고 닫는다 (accept 루프는 절대 블록하지 않음).
 */
#ifndef SERVER_H
#define SERVER_H

#include <signal.h>

#include "threadpool.h"

/*
 * socket → SO_REUSEADDR → bind → listen. host 는 IPv4 문자열("0.0.0.0", "127.0.0.1").
 * 실패하면 원인(errno 포함)을 로그로 남기고 -1. 성공하면 listen fd.
 */
int server_listen(const char *host, int port, int backlog);

/* *stop 이 설정될 때까지 accept 해서 tp 에 넣는다. 정상 종료 0, 복구 불가 오류 -1. */
int server_run(int lfd, threadpool_t *tp, volatile sig_atomic_t *stop);

#endif /* SERVER_H */
