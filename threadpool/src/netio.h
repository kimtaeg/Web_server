/*
 * netio.h — 소켓 I/O 헬퍼. 프로젝트의 모든 send/recv 는 이 함수들만 거친다.
 *
 * [왜 헬퍼로 강제하는가]
 * send 는 요청한 바이트를 다 보내지 않고 돌아올 수 있고(부분 전송),
 * send/recv/poll 은 시그널에 의해 EINTR 로 중단될 수 있다.
 * 이 처리를 호출하는 곳마다 반복하면 반드시 어딘가에서 빠뜨린다. 한 곳에 모은다.
 * SIGPIPE 방어 플래그(PLATFORM_SEND_FLAGS)도 여기서 일괄 적용된다.
 */
#ifndef NETIO_H
#define NETIO_H

#include <stddef.h>
#include <sys/types.h>

#define NETIO_TIMEOUT (-2)

/* n 바이트를 전부 보낸다. 성공 시 n, 실패 시 -1 (SO_SNDTIMEO 만료 포함). EINTR 은 재시도. */
ssize_t send_all(int fd, const void *p, size_t n);

/*
 * 한 번 받는다. timeout_ms 동안 poll 로 기다린 뒤 recv.
 * 반환: 받은 바이트 수(>0) / 0 = 상대가 연결 종료(EOF) / -1 = 오류 / NETIO_TIMEOUT.
 * timeout_ms < 0 이면 무한 대기.
 */
ssize_t recv_some(int fd, void *p, size_t n, int timeout_ms);

/*
 * 블로킹하지 않고 한 번만 보내 본다 (best-effort). 보낸 바이트 수 또는 -1.
 * 메인(accept) 스레드가 503 을 보낼 때 쓴다: 응답을 읽지 않는 클라이언트 때문에
 * accept 루프가 멈추면 서버 전체가 멈추기 때문.
 * 주의: fd 를 O_NONBLOCK 으로 바꾼다. 보낸 뒤 바로 닫을 fd 에만 쓴다.
 */
ssize_t send_nowait(int fd, const void *p, size_t n);

/* SO_SNDTIMEO 설정. 응답을 읽지 않는 클라이언트가 워커를 영원히 붙잡지 못하게 한다. 0/-1 */
int netio_set_send_timeout(int fd, int timeout_ms);

/*
 * SO_RCVTIMEO 설정. recv_some 은 poll 로 기다리므로 필요 없지만, TLS 는 OpenSSL 이 소켓을 직접
 * read 한다: 레코드 일부만 도착한 상태에서 SSL_read 가 영원히 블록하지 않도록 상한을 건다. 0/-1
 */
int netio_set_recv_timeout(int fd, int timeout_ms);

#endif /* NETIO_H */
