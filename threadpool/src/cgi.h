/*
 * cgi.h — CGI/1.1 (RFC 3875) 실행.
 *
 *   요청 → 스크립트 결정 → fork → (자식) 파이프 2개 dup2 → execve
 *                              (부모) poll 로 stdin 쓰기와 stdout 읽기를 동시에 → waitpid
 *        → CGI 출력 헤더(Status, Content-Type, Location) 해석 → HTTP 응답
 *
 * [멀티스레드에서 fork 하기]
 * fork 는 호출한 스레드 하나만 자식에 복제한다. 다른 스레드가 잡고 있던 락(malloc 내부 락, 로그 락)은
 * 자식에서 영원히 풀리지 않으므로, 자식은 exec 전까지 async-signal-safe 함수만 써야 한다
 * (dup2, chdir, setpgid, sigaction, execve, _exit). 그래서 argv/envp 는 fork 전에 부모가 만든다.
 *
 * [fd 누수 방지]
 * 다른 워커가 동시에 fork 하면 그 순간 열려 있던 모든 fd 가 CGI 로 복제된다. 클라이언트 소켓이
 * 새면 CGI 가 끝날 때까지 연결이 닫히지 않는다. 그래서 서버의 모든 fd 에 CLOEXEC 를 건다
 * (리슨/클라이언트 소켓, 정적 파일, CGI 파이프).
 *
 * [교착 방지]
 * 본문을 다 쓴 뒤 출력을 읽으면, 출력을 먼저 많이 쓰는 CGI 와 서로 파이프 버퍼(64KB)가 차서
 * 영원히 기다린다. 두 파이프를 non-blocking 으로 두고 한 poll 루프에서 동시에 처리한다.
 *
 * [타임아웃과 좀비]
 * 자식은 setpgid 로 자기 프로세스 그룹을 만든다. 타임아웃이면 kill(-pgid, SIGKILL) 로 그룹 전체
 * (셸 스크립트가 띄운 손자 포함)를 죽이고 waitpid 로 회수한 뒤 504.
 * 모든 경로에서 waitpid(pid) 를 정확히 한 번 부른다 → 좀비가 남지 않는다.
 * waitpid(-1) 이 아니라 pid 를 지정하므로 다른 워커의 자식을 가로채지 않는다.
 *
 * 전제: 프로세스가 SIGPIPE 를 무시하고 있어야 한다 (main 이 설정). CGI 가 stdin 을 안 읽고 끝나면
 * 파이프 write 가 SIGPIPE 를 일으키기 때문. 파이프에는 MSG_NOSIGNAL 같은 플래그가 없다.
 */
#ifndef CGI_H
#define CGI_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#include "buf.h"
#include "conn.h"

#define CGI_DEFAULT_TIMEOUT_MS 5000
#define CGI_MAX_OUTPUT (8 * 1024 * 1024) /* 출력을 버퍼에 모아 Content-Length 를 붙이므로 상한 필요 */

typedef enum {
    CGI_OK = 0,
    CGI_ERR_INTERNAL = -1, /* pipe/fork 실패 등 서버 쪽 문제 → 500 */
    CGI_ERR_TIMEOUT = -2,  /* → 504 */
    CGI_ERR_EXIT = -3,     /* 0 이 아닌 종료 코드 또는 시그널로 종료 → 500 */
    CGI_ERR_EXEC = -4,     /* execve 실패 (종료 코드 127) → 500 */
    CGI_ERR_TOO_BIG = -5   /* 출력이 상한 초과 → 500 */
} cgi_rc_t;

typedef struct {
    char dir[PATH_MAX];     /* realpath 로 푼 CGI 디렉터리 */
    size_t dir_len;
    char prefix[256];       /* URL 접두사, 예: "/cgi-bin" (끝 '/' 없음) */
    size_t prefix_len;
    int timeout_ms;
    size_t max_output;
} cgi_ctx_t;

/* dir 이 존재하는 디렉터리가 아니면 -1. */
int cgi_init(cgi_ctx_t *c, const char *dir, const char *url_prefix, int timeout_ms);

/* ---- 순수 함수 (단위 테스트 대상) ---- */

/*
 * 스크립트 이름 검사: 경로 구성요소 하나, 문자는 [A-Za-z0-9._-], "." ".." 와 숨김 파일 금지.
 * 퍼센트 인코딩은 허용하지 않는다('%' 금지) → 디코딩 우회 자체가 불가능하다.
 */
int cgi_script_name_ok(const char *name, size_t n);

/*
 * CGI 출력 해석. 헤더 줄은 "\n" 또는 "\r\n" 로 끝나고 빈 줄에서 본문이 시작된다.
 *   Status: 404 Not Found  → *status = 404
 *   Location 만 있고 Status 없음 → 302
 *   Content-Type 도 Location 도 없으면 오류 (RFC 3875 6.2)
 * 응답에 그대로 넘길 헤더를 "Name: value\r\n" 형식으로 headers 에 쓴다.
 * 서버가 직접 정하는 헤더(Connection, Content-Length, Transfer-Encoding, Date, Server, Status)는 뺀다.
 * 반환 0 / 형식 오류 -1.
 */
int cgi_parse_output(const char *out, size_t n, int *status, buf_t *headers, size_t *body_off);

/* ---- 실행 ---- */

/*
 * path 를 실행하고 stdout 전체를 out 에 모은다. 작업 디렉터리는 스크립트가 있는 디렉터리.
 * pid_out 이 NULL 이 아니면 자식 pid 를 알려 준다 (테스트에서 회수 여부 확인용).
 * 반환: cgi_rc_t.
 */
int cgi_run(const char *path, char *const envp[], const char *body, size_t body_len, int timeout_ms,
            size_t max_output, buf_t *out, pid_t *pid_out);

/* conn_handler_fn. GET/HEAD/POST 만 허용 (그 외 405). */
int cgi_handle(void *ctx, conn_t *cl, const http_request_t *req, const conn_info_t *ci, int keep_alive,
               handler_result_t *res);

#endif /* CGI_H */
