/*
 * log.c — 레벨별 로그. 동기화 방식은 log.h 참고 (전역 mutex 로 한 줄 단위 직렬화).
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <time.h>

#define LOG_TARGET_MAX 256 /* 접근 로그에 남길 대상 경로 최대 길이 */

/* 아래 세 변수는 모두 log_mu 로 보호한다. */
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_out; /* NULL = stderr */
static log_level_t log_min = LOG_INFO;

static const char *const level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void log_init(log_level_t min_level, FILE *out)
{
    pthread_mutex_lock(&log_mu);
    log_min = min_level;
    log_out = out;
    pthread_mutex_unlock(&log_mu);
}

static void format_time(char *b, size_t n)
{
    struct timespec ts;
    struct tm tm;
    size_t k;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm); /* localtime 은 정적 버퍼를 써서 스레드 안전하지 않다 */
    k = strftime(b, n, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(b + k, n - k, ".%03ld", ts.tv_nsec / 1000000L);
}

static void vlog(log_level_t lvl, const char *fmt, va_list ap)
{
    char ts[40];
    FILE *out;

    format_time(ts, sizeof ts);
    pthread_mutex_lock(&log_mu);
    if (lvl >= log_min) {
        out = log_out ? log_out : stderr;
        fprintf(out, "%s [%s] ", ts, level_names[lvl]);
        vfprintf(out, fmt, ap);
        fputc('\n', out);
        fflush(out);
    }
    pthread_mutex_unlock(&log_mu);
}

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog(lvl, fmt, ap);
    va_end(ap);
}

void log_access(const char *ip, const char *method, const char *target, size_t target_len,
                int version_minor, int status, size_t bytes, long ms)
{
    /* target 은 파서가 출력 가능한 ASCII 만 허용했으므로 로그 주입(개행 삽입) 걱정이 없다. */
    int tl = (int)(target_len > LOG_TARGET_MAX ? LOG_TARGET_MAX : target_len);

    if (method == NULL) {
        log_msg(LOG_INFO, "%s \"-\" %d %zuB %ldms", ip ? ip : "-", status, bytes, ms);
        return;
    }
    log_msg(LOG_INFO, "%s \"%s %.*s%s HTTP/1.%d\" %d %zuB %ldms", ip ? ip : "-", method, tl, target,
            target_len > LOG_TARGET_MAX ? "..." : "", version_minor, status, bytes, ms);
}
