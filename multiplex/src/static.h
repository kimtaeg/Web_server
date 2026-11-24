/*
 * static.h — 정적 파일 응답 계획과 경로 처리 순수 함수.
 *
 * [루트 탈출 방어: 두 겹]
 * 1. 문자열 단계 (순수 함수, 파일시스템 접근 없음):
 *    퍼센트 디코딩 → "//", ".", ".." 정규화. ".." 가 루트 위로 올라가면 즉시 403.
 *    "%2e%2e" 는 디코딩 후 ".." 가 되므로 반드시 "디코딩 → 정규화" 순서여야 한다.
 * 2. 파일시스템 단계: realpath 로 심볼릭 링크까지 푼 실제 경로가 루트 아래인지 다시 검사.
 *    문자열만으로는 "루트 안의 심볼릭 링크가 밖을 가리키는" 경우를 잡을 수 없기 때문.
 * 둘 중 하나만으로는 부족하다. 1 은 링크를 모르고, 2 만 쓰면 존재하지 않는 경로에서
 * realpath 가 ENOENT 로 실패해 404 가 나가므로 "탈출 시도 = 403" 정책을 지킬 수 없다.
 */
#ifndef STATIC_H
#define STATIC_H

#include <limits.h>
#include <stddef.h>

#include "buf.h"
#include "http_parser.h"

#define STATIC_EBAD (-1)     /* 잘못된 퍼센트 인코딩 또는 %00 → 400 */
#define STATIC_ETOOLONG (-2) /* 출력 버퍼 부족 → 414 */

/* ---- 순수 함수 (단위 테스트 대상) ---- */

/*
 * 경로의 퍼센트 디코딩. '+' 는 공백으로 바꾸지 않는다 (그건 쿼리 문자열 규칙).
 * %00 은 C 문자열을 잘라 검사를 우회하는 데 쓰이므로 거부한다.
 * 반환: 디코딩된 길이(out 은 NUL 종료) / STATIC_EBAD / STATIC_ETOOLONG.
 */
int static_url_decode(const char *in, size_t n, char *out, size_t outsz);

/*
 * '/' 로 시작하는 경로를 제자리에서 정규화: 연속 '/' 합치기, "." 제거, ".." 로 한 단계 위로.
 * 결과는 항상 '/' 로 시작하고 끝의 '/' 는 없다 (루트는 "/").
 * ".." 가 루트 위로 올라가면 -1 (탈출 시도), 성공 0.
 */
int static_path_normalize(char *path);

/*
 * resolved 가 root 와 같거나 root 아래인지. 단순 접두사 비교는 "/var/www" 가 "/var/www2" 를
 * 포함한다고 오판하므로, 접두사 다음 글자가 '\0' 또는 '/' 인지까지 본다.
 */
int static_path_within(const char *root, size_t root_len, const char *resolved);

/* ---- 핸들러 ---- */

typedef struct {
    char root[PATH_MAX]; /* realpath 로 푼 절대 경로 */
    size_t root_len;
} static_ctx_t;

/* root 를 realpath 로 풀어 저장한다. 존재하는 디렉터리가 아니면 -1. */
int static_init(static_ctx_t *s, const char *root);

#define STATIC_HEAD_MAX (32 * 1024) /* 응답 헤더(오류면 상태 페이지 본문 포함) 상한 */

/*
 * 응답 하나의 "계획". 이벤트 루프는 head 를 먼저 보내고, file_len > 0 이면 file_fd 에서
 * file_len 바이트를 읽어 이어 보낸다. 소켓 I/O 가 없으므로 단위 테스트에서 결과를 바로 검사할 수 있다.
 *
 * 왜 핸들러가 직접 보내지 않나: 이벤트 루프에서 send 가 EAGAIN 이면 기다리지 않고 다른 연결로
 * 넘어가야 한다. "어디까지 보냈는지"를 함수 스택이 아니라 연결 구조체에 남겨야 하므로,
 * 보낼 내용을 데이터로 만들어 넘기는 구조가 된다 (threadpool/ 판은 워커가 send 에서 블록해도 된다).
 */
typedef struct {
    int status;      /* 접근 로그용 */
    buf_t head;      /* 상태줄 + 헤더 + 빈 줄 (상태 페이지면 HTML 본문까지) */
    int file_fd;     /* 본문 파일. -1 이면 없음. 소유권은 계획에 있고 static_plan_free 가 닫는다 */
    size_t file_len; /* file_fd 에서 보낼 바이트 수 (HEAD 면 0) */
} static_plan_t;

void static_plan_init(static_plan_t *p);
void static_plan_free(static_plan_t *p);

/*
 * GET/HEAD 만 허용 (그 외 405 + Allow). 어떤 경우든 p 에 보낼 응답을 채운다(오류는 상태 페이지).
 * 반환: 0, 또는 500 페이지조차 만들 수 없을 때(메모리 부족) -1 — 호출자는 연결을 닫는다.
 */
int static_prepare(const static_ctx_t *s, const http_request_t *req, int keep_alive,
                   static_plan_t *p);

#endif /* STATIC_H */
