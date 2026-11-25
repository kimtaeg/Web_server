/*
 * test_config.c — host:port 파싱, 설정 파싱(정상/오류 메시지의 줄 번호), 파일 로드.
 */
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "config.h"

static void test_hostport(void)
{
    static const struct {
        const char *in;
        int rc;
        const char *host;
        int port;
    } cases[] = {
        {"127.0.0.1:8080", 0, "127.0.0.1", 8080},
        {"localhost:1", 0, "localhost", 1},
        {"backend.internal:65535", 0, "backend.internal", 65535},
        {"[::1]:9000", 0, "::1", 9000},
        {"127.0.0.1", -1, NULL, 0},     /* 포트 없음 */
        {"127.0.0.1:", -1, NULL, 0},
        {":80", -1, NULL, 0},           /* 호스트 없음 */
        {"h:0", -1, NULL, 0},
        {"h:65536", -1, NULL, 0},
        {"h:80x", -1, NULL, 0},
        {"h:-1", -1, NULL, 0},
        {"::1:80", -1, NULL, 0},        /* 대괄호 없는 IPv6 는 모호 */
        {"[::1]80", -1, NULL, 0},
        {"[::1", -1, NULL, 0},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        config_hostport_t hp;
        int rc = config_parse_hostport(cases[i].in, &hp);

        if (rc != cases[i].rc)
            fprintf(stderr, "    hostport(%s) = %d\n", cases[i].in, rc);
        CHECK_EQ_INT(rc, cases[i].rc);
        if (rc == 0) {
            CHECK_EQ_STR(hp.host, cases[i].host);
            CHECK_EQ_INT(hp.port, cases[i].port);
        }
    }
}

static void test_parse_full(void)
{
    static config_t c;
    char err[256] = "";
    const char *text = "# 예시 설정\n"
                       "listen 127.0.0.1:9090\n"
                       "root   /srv/www   # 주석\n"
                       "\n"
                       "cgi /cgi-bin /srv/cgi\n"
                       "workers 4\n"
                       "queue\t\t32\n"
                       "upstream_connect_timeout_ms 250\n"
                       "upstream_read_timeout_ms 1500\n"
                       "upstream_down_secs 3\n"
                       "route /api 127.0.0.1:9001 127.0.0.1:9002\n"
                       "route host=Api.Local / [::1]:9003\n"
                       "route /   10.0.0.1:80\r\n" /* CRLF 줄도 허용 */
                       "forward_proxy on\n"
                       "cache_max_bytes 1000000\n"
                       "cache_max_object_bytes 5000\n"
                       "cache_default_ttl_secs 0\n"
                       "https_port 9443\n"
                       "tls_cert /etc/x/server.crt\n"
                       "tls_key  /etc/x/server.key\n";

    config_defaults(&c);
    CHECK_EQ_INT((int)c.https_port, 8443); /* 기본값 */
    CHECK_EQ_STR(c.tls_cert, "");          /* 인증서가 없으면 HTTPS 꺼짐 */
    CHECK_EQ_INT(c.forward_proxy, 0); /* 기본값: 포워드 프록시 꺼짐 (open proxy 방지) */
    CHECK_EQ_INT(config_parse_string(text, &c, err, sizeof err), 0);
    CHECK_EQ_STR(err, "");
    CHECK_EQ_STR(c.listen_host, "127.0.0.1");
    CHECK_EQ_INT((int)c.https_port, 9443);
    CHECK_EQ_STR(c.tls_cert, "/etc/x/server.crt");
    CHECK_EQ_STR(c.tls_key, "/etc/x/server.key");
    CHECK_EQ_INT(c.listen_port, 9090);
    CHECK_EQ_STR(c.root, "/srv/www");
    CHECK_EQ_STR(c.cgi_prefix, "/cgi-bin");
    CHECK_EQ_STR(c.cgi_dir, "/srv/cgi");
    CHECK_EQ_INT(c.workers, 4);
    CHECK_EQ_INT(c.queue, 32);
    CHECK_EQ_INT(c.idle_timeout_ms, 5000); /* 기본값 유지 */
    CHECK_EQ_INT(c.upstream_connect_timeout_ms, 250);
    CHECK_EQ_INT(c.upstream_read_timeout_ms, 1500);
    CHECK_EQ_INT(c.upstream_down_secs, 3);
    REQUIRE(c.nroutes == 3);
    CHECK_EQ_STR(c.routes[0].host, "");
    CHECK_EQ_STR(c.routes[0].prefix, "/api");
    CHECK_EQ_SIZE(c.routes[0].nbackends, 2);
    CHECK_EQ_STR(c.routes[0].backends[1].host, "127.0.0.1");
    CHECK_EQ_INT(c.routes[0].backends[1].port, 9002);
    CHECK_EQ_STR(c.routes[1].host, "Api.Local");
    CHECK_EQ_STR(c.routes[1].prefix, "/");
    CHECK_EQ_STR(c.routes[1].backends[0].host, "::1");
    CHECK_EQ_STR(c.routes[2].backends[0].host, "10.0.0.1");
    CHECK_EQ_INT(c.forward_proxy, 1);
    CHECK_EQ_INT(c.cache_max_bytes, 1000000);
    CHECK_EQ_INT(c.cache_max_object_bytes, 5000);
    CHECK_EQ_INT(c.cache_default_ttl_secs, 0);
}

static void test_parse_errors(void)
{
    static const struct {
        const char *text;
        const char *msg; /* err 에 포함돼야 할 문자열 */
    } cases[] = {
        {"rout /a 1.2.3.4:80\n", "line 1: 알 수 없는 지시어 'rout'"},
        {"\n\n# c\nworkers abc\n", "line 4: 'workers' 의 값 'abc'"},
        {"workers 0\n", "line 1:"},
        {"workers\n", "line 1: 'workers' 는 값 하나가 필요하다"},
        {"workers 1 2\n", "line 1:"},
        {"listen 8080\n", "line 1: listen"},
        {"route /a\n", "line 1: route /a 에 백엔드"},
        {"route api 1.2.3.4:80\n", "line 1: route 는 '/' 로 시작"},
        {"route host= / 1.2.3.4:80\n", "host= 값"},
        {"route /a 1.2.3.4\n", "line 1: 백엔드 주소 '1.2.3.4'"},
        {"route /a 1.2.3.4:80 bad\n", "백엔드 주소 'bad'"},
        {"cgi cgi-bin /x\n", "line 1: cgi"},
        {"root\n", "line 1: root"},
        {"ok_line_is_not_ok\n", "line 1:"},
        {"workers 2\nqueue 2\nfoo\n", "line 3:"},
        {"forward_proxy yes\n", "line 1: forward_proxy 는 on 또는 off"},
        {"cache_max_object_bytes 0\n", "line 1:"},
        {"https_port 70000\n", "line 1: 'https_port'"},
        {"tls_cert\n", "line 1: tls_cert 는 경로"},
        {"tls_key a b\n", "line 1: tls_key 는 경로"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static config_t c;
        char err[256] = "";

        config_defaults(&c);
        CHECK_EQ_INT(config_parse_string(cases[i].text, &c, err, sizeof err), -1);
        if (strstr(err, cases[i].msg) == NULL)
            fprintf(stderr, "    기대 [%s] 실제 [%s]\n", cases[i].msg, err);
        CHECK(strstr(err, cases[i].msg) != NULL);
    }
}

static void test_too_many(void)
{
    static config_t c;
    char text[8192];
    char err[256];
    size_t off = 0;
    int i;

    for (i = 0; i <= CONFIG_MAX_ROUTES; i++)
        off += (size_t)snprintf(text + off, sizeof text - off, "route /r%d 1.1.1.1:80\n", i);
    config_defaults(&c);
    CHECK_EQ_INT(config_parse_string(text, &c, err, sizeof err), -1);
    CHECK(strstr(err, "route 는 최대") != NULL);

    off = (size_t)snprintf(text, sizeof text, "route /a");
    for (i = 0; i <= CONFIG_MAX_BACKENDS; i++)
        off += (size_t)snprintf(text + off, sizeof text - off, " 1.1.1.1:%d", 1000 + i);
    snprintf(text + off, sizeof text - off, "\n");
    config_defaults(&c);
    CHECK_EQ_INT(config_parse_string(text, &c, err, sizeof err), -1);
}

static void test_load_file(void)
{
    static config_t c;
    char path[] = "/tmp/cweb-conf-XXXXXX";
    int fd = mkstemp(path);
    char err[256] = "";
    const char *text = "listen 0.0.0.0:7000\nroute /x 127.0.0.1:1\n";

    REQUIRE(fd >= 0);
    REQUIRE(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    config_defaults(&c);
    CHECK_EQ_INT(config_load(path, &c, err, sizeof err), 0);
    CHECK_EQ_INT(c.listen_port, 7000);
    CHECK_EQ_SIZE(c.nroutes, 1);
    unlink(path);

    CHECK_EQ_INT(config_load("/no/such/file.conf", &c, err, sizeof err), -1);
    CHECK(strstr(err, "열 수 없음") != NULL);
}

int main(void)
{
    RUN_TEST(test_hostport);
    RUN_TEST(test_parse_full);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_too_many);
    RUN_TEST(test_load_file);
    TEST_END();
}
