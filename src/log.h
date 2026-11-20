/*
 * log.h — 레벨별 로그와 접근 로그.
 *
 * 동기화: 전역 mutex 하나로 "한 줄 쓰기"를 통째로 직렬화한다.
 * 여러 워커가 동시에 쓰면 한 줄이 다른 줄과 섞이기 때문 (stdio 의 내부 락은 호출 단위라
 * fprintf 여러 번으로 한 줄을 만들면 그 사이에 끼어들 수 있다).
 * 로그는 요청당 한 줄이라 경합 비용은 작다.
 */
#ifndef LOG_H
#define LOG_H

#include <stddef.h>
#include <stdio.h>

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

/* out 이 NULL 이면 stderr. 스레드를 만들기 전에 호출하는 것을 권장하지만 언제 불러도 안전하다. */
void log_init(log_level_t min_level, FILE *out);

void log_msg(log_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* "GET /index.html 200 1234B 3ms" 형식의 접근 로그 (INFO). target 은 길면 잘라서 기록. */
void log_access(const char *method, const char *target, size_t target_len, int status,
                size_t bytes, long ms);

#endif /* LOG_H */
