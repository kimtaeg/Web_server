/*
 * static.h — 정적 파일 핸들러와 경로 처리 순수 함수.
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

#include "conn.h"

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

/* conn_handler_fn. GET/HEAD 만 허용 (그 외 405 + Allow). */
int static_handle(void *ctx, int fd, const http_request_t *req, const conn_info_t *ci,
                  int keep_alive, handler_result_t *res);

#endif /* STATIC_H */
