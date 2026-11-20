/*
 * platform.h — macOS / Linux 차이를 흡수하는 유일한 장소.
 *
 * 규칙: 다른 모듈에는 #ifdef __APPLE__ / __linux__ 를 쓰지 않는다.
 * OS 분기가 필요하면 여기에 함수/매크로를 하나 추가하고 그것을 호출한다.
 * 별도 .c 없이 static inline 으로만 둬서 "OS 차이 = 이 파일 하나"를 유지한다.
 *
 * [SIGPIPE 를 왜 두 겹으로 막는가]
 * 상대가 끊은 소켓에 send 하면 커널이 SIGPIPE 를 보내고, 기본 동작은 프로세스 종료다.
 * 워커 하나의 실수로 서버 전체가 죽으면 안 되므로:
 *   1) main 에서 SIGPIPE 를 SIG_IGN (프로세스 전체 방어선)
 *   2) 소켓 단위로도 막는다 — 시그널 설정에 의존하지 않고 send 가 EPIPE 로 돌아오게.
 * 2)의 방식이 OS 마다 다르다:
 *   - Linux : send 플래그 MSG_NOSIGNAL (호출마다 지정)
 *   - macOS : MSG_NOSIGNAL 이 없고, 소켓 옵션 SO_NOSIGPIPE (소켓마다 한 번 설정)
 * 그래서 두 가지를 모두 추상화한다. 호출 측은 항상 아래 두 가지를 같이 쓴다:
 *   - 소켓을 만들거나 accept 한 직후 platform_sock_init(fd)
 *   - send(..., PLATFORM_SEND_FLAGS)
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <sys/socket.h>

#if defined(__linux__)

#define PLATFORM_SEND_FLAGS MSG_NOSIGNAL

/* Linux 는 send 플래그로 막으므로 소켓 단위 설정이 필요 없다. 0=성공 */
static inline int platform_sock_init(int fd)
{
    (void)fd;
    return 0;
}

#elif defined(__APPLE__)

/* SO_NOSIGPIPE 는 _DARWIN_C_SOURCE 에서만 선언된다 (Makefile 이 정의). */
#ifndef SO_NOSIGPIPE
#error "SO_NOSIGPIPE 가 보이지 않음: -D_DARWIN_C_SOURCE 로 빌드해야 한다"
#endif

#define PLATFORM_SEND_FLAGS 0

/* accept/socket 직후 호출. 0=성공, -1=실패(errno 설정) */
static inline int platform_sock_init(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

#else
#error "지원하지 않는 플랫폼 (Linux, macOS 만 지원)"
#endif

/* sendfile 시그니처 차이는 정적 파일을 구현하는 2단계에서 여기에 추가한다. */

#endif /* PLATFORM_H */
