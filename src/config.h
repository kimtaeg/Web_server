/*
 * config.h — 설정 파일 파싱.
 *
 * 형식: 한 줄에 지시어 하나, 공백으로 구분, '#' 뒤는 주석. 경로에 공백은 쓸 수 없다.
 *
 *   listen   127.0.0.1:8080
 *   root     ./www
 *   cgi      /cgi-bin ./build/release/cgi-bin
 *   workers 8 | queue 64 | idle_timeout_ms 5000 | max_requests 100 | cgi_timeout_ms 5000
 *   upstream_connect_timeout_ms 1000
 *   upstream_read_timeout_ms    5000
 *   upstream_down_secs          10
 *   route    /api              127.0.0.1:9001 127.0.0.1:9002   # 경로 접두사 규칙
 *   route    host=api.local /  127.0.0.1:9003                  # Host 헤더 규칙
 *
 * 오류는 "line 7: unknown directive 'rout'" 처럼 줄 번호와 함께 err 에 쓴다.
 * 동기화: 시작 시 main 스레드에서만 읽고 쓴다. 이후 서버는 여기서 만든 값을 복사해 쓴다.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <stddef.h>

#define CONFIG_MAX_ROUTES 16
#define CONFIG_MAX_BACKENDS 16
#define CONFIG_MAX_NAME 256

typedef struct {
    char host[CONFIG_MAX_NAME]; /* "127.0.0.1", "localhost", "::1" (대괄호 제거) */
    int port;
} config_hostport_t;

typedef struct {
    char host[CONFIG_MAX_NAME]; /* Host 헤더 조건. "" = 모든 Host */
    char prefix[CONFIG_MAX_NAME];
    config_hostport_t backends[CONFIG_MAX_BACKENDS];
    size_t nbackends;
} config_route_t;

typedef struct {
    char listen_host[64];
    int listen_port;
    char root[PATH_MAX];
    char cgi_prefix[CONFIG_MAX_NAME]; /* "" = CGI 비활성 */
    char cgi_dir[PATH_MAX];
    long workers;
    long queue;
    long idle_timeout_ms;
    long max_requests;
    long cgi_timeout_ms;
    long upstream_connect_timeout_ms;
    long upstream_read_timeout_ms;
    long upstream_down_secs;
    config_route_t routes[CONFIG_MAX_ROUTES];
    size_t nroutes;
} config_t;

void config_defaults(config_t *c);

/* "host:port", "[::1]:8080". 순수 함수. 성공 0. */
int config_parse_hostport(const char *s, config_hostport_t *out);

/* 설정 텍스트 전체를 파싱해 c 에 덮어쓴다 (순수 함수: 단위 테스트 대상). 성공 0, 실패 -1. */
int config_parse_string(const char *text, config_t *c, char *err, size_t errsz);

/* 파일을 읽어 config_parse_string. */
int config_load(const char *path, config_t *c, char *err, size_t errsz);

#endif /* CONFIG_H */
