/*
 * buf.h — 상한(max)이 있는 가변 바이트 버퍼.
 *
 * 용도: 소켓에서 읽은 요청 바이트를 쌓아 두고, 파서가 소비한 만큼 앞에서 덜어낸다.
 *
 * [왜 상한을 버퍼에 두는가]
 * 헤더 16KB 초과 → 431, 본문 초과 → 413 처럼 "얼마나 쌓였는가"가 곧 오류 정책이다.
 * append 가 상한을 넘으면 메모리 부족(BUF_ERR_NOMEM)과 구분되는 BUF_ERR_LIMIT 를
 * 돌려주므로, 호출자는 500 과 413/431 을 정확히 나눠 응답할 수 있다.
 * 상한이 없으면 악의적인 클라이언트가 헤더를 끝없이 보내 메모리를 고갈시킬 수 있다.
 *
 * 불변식:
 *   - len <= max
 *   - data != NULL 이면 len < cap 이고 data[len] == '\0'
 *     (바이너리도 담을 수 있지만, 디버깅/로그용으로 항상 NUL 종료를 보장한다)
 *
 * 스레드 안전성: 없음. buf_t 하나는 연결 하나(= 워커 하나)만 소유한다.
 */
#ifndef BUF_H
#define BUF_H

#include <stddef.h>
#include <sys/types.h> /* ssize_t */

enum {
    BUF_OK = 0,
    BUF_ERR_NOMEM = -1, /* malloc/realloc 실패 → 500 */
    BUF_ERR_LIMIT = -2, /* max 초과 → 호출자가 413/431 등으로 매핑 */
    BUF_ERR_FMT = -3    /* buf_appendf 의 형식 오류 */
};

typedef struct {
    char *data;
    size_t len; /* 유효 바이트 수 */
    size_t cap; /* 할당 크기 (NUL 1바이트 포함) */
    size_t max; /* len 의 상한 */
} buf_t;

/* init_cap 만큼 미리 할당 (0 이면 첫 append 때 할당). max_cap 은 1 이상이어야 한다. */
int buf_init(buf_t *b, size_t init_cap, size_t max_cap);
void buf_free(buf_t *b);

/* 실패하면 버퍼 내용은 호출 전과 같다 (부분 기록 없음). */
int buf_append(buf_t *b, const void *p, size_t n);
int buf_appendf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 앞에서 n 바이트 제거. n >= len 이면 비운다. */
void buf_consume(buf_t *b, size_t n);
void buf_reset(buf_t *b);

/*
 * data[from..len) 에서 needle 의 첫 위치(버퍼 시작 기준 오프셋)를 찾는다. 없으면 -1.
 *
 * from 은 재검색 비용을 줄이기 위한 것이다. 데이터가 조각으로 도착할 때
 * 매번 처음부터 "\r\n\r\n" 을 찾으면 O(n^2) 이 되므로, 이전에 검사한 길이를
 * prev 라 할 때 from = prev >= nlen-1 ? prev-(nlen-1) : 0 으로 넘기면
 * 경계에 걸친 needle 도 놓치지 않으면서 새 바이트만 본다.
 */
ssize_t buf_find(const buf_t *b, size_t from, const void *needle, size_t nlen);

#endif /* BUF_H */
