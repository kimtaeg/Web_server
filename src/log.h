#ifndef LOG_H
#define LOG_H

void log_set_access(int enabled);
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_access(const char *peer, const char *method, const char *target, int version_minor,
                int status, long long bytes, long long elapsed_us, const char *handler,
                const char *upstream);

#endif
