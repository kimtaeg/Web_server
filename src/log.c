#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int access_enabled = 1;

void log_set_access(int enabled) {
    access_enabled = enabled;
}

static void stamp(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static void vlog(FILE *f, const char *level, const char *fmt, va_list ap) {
    char ts[32];
    stamp(ts, sizeof(ts));
    pthread_mutex_lock(&log_lock);
    fprintf(f, "%s [%s] ", ts, level);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    pthread_mutex_unlock(&log_lock);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, "info", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, "error", fmt, ap);
    va_end(ap);
}

void log_access(const char *peer, const char *method, const char *target, int version_minor,
                int status, long long bytes, long long elapsed_us, const char *handler,
                const char *upstream) {
    if (!access_enabled)
        return;
    char ts[32];
    stamp(ts, sizeof(ts));
    pthread_mutex_lock(&log_lock);
    fprintf(stdout, "%s %s \"%s %.200s HTTP/1.%d\" %d %lld %.2fms %s%s%s\n", ts, peer,
            method ? method : "-", target ? target : "-", version_minor, status, bytes,
            (double)elapsed_us / 1000.0, handler, upstream && *upstream ? " -> " : "",
            upstream ? upstream : "");
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}
