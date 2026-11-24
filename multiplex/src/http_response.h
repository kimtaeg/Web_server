/*
 * http_response.h — 응답 상태줄·헤더 직렬화와 간단한 HTML 상태 페이지.
 *
 * 모든 응답은 Content-Length 를 명시한다 (chunked 응답 없음).
 * 길이를 알려야 keep-alive 연결에서 클라이언트가 응답의 끝을 알 수 있다.
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <time.h>

#include "buf.h"

#define HTTP_SERVER_NAME "cweb/0.2"
/* "Sun, 06 Nov 1994 08:49:37 GMT" 은 29자지만, gcc -Wformat-truncation 은 int 필드를 최악(11자리)으로
 * 계산하므로 그 경우까지 들어가는 크기로 둔다 */
#define HTTP_DATE_LEN 80

const char *http_status_reason(int status);

/*
 * 백엔드 모드: 설정하면 이후 모든 응답 헤더에 "X-Backend: <name>" 이 붙는다 (로드밸런싱 확인용).
 * 동기화: 워커 스레드를 만들기 전에 main 이 한 번만 호출한다. 이후 읽기 전용이라 락이 없다.
 * NULL 이면 해제.
 */
void http_response_set_backend_name(const char *name);

/* RFC 9110 5.6.7 IMF-fixdate. locale 에 의존하지 않도록 요일/월 이름을 직접 쓴다. */
void http_date(char out[HTTP_DATE_LEN], time_t t);

/*
 * 상태줄 + Date/Server/Content-Type/Content-Length/Connection + extra_headers + 빈 줄을 out 에 덧붙인다.
 * ctype 이 NULL 이면 Content-Type 생략. extra_headers 는 "Name: value\r\n" 들의 연결(NULL 가능).
 * 반환: BUF_OK 또는 buf 오류 코드.
 */
int http_response_head(buf_t *out, int status, const char *ctype, size_t content_length,
                       int keep_alive, const char *extra_headers);

/* 상태 코드를 제목으로 하는 작은 HTML 페이지 응답 전체(헤더+본문)를 out 에 만든다. head_only 면 본문 생략. */
int http_response_build_status_page(buf_t *out, int status, int keep_alive, int head_only,
                                    const char *extra_headers);

/* 위 페이지를 fd 로 보낸다. *sent 에 보낸 바이트 수. 반환 0 / -1(전송 실패). */
int http_response_status_page(int fd, int status, int keep_alive, int head_only,
                              const char *extra_headers, size_t *sent);

#endif /* HTTP_RESPONSE_H */
