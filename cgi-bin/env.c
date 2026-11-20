/*
 * cgi-bin/env.c — 예제 CGI: 서버가 넘겨 준 CGI 환경 변수를 "NAME=value" 줄로 출력 (검증용).
 */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    static const char *const names[] = {
        "REQUEST_METHOD", "QUERY_STRING", "CONTENT_LENGTH", "CONTENT_TYPE", "SCRIPT_NAME",
        "SERVER_PROTOCOL", "REMOTE_ADDR", "GATEWAY_INTERFACE", "SERVER_SOFTWARE",
        "HTTP_HOST", "HTTP_X_FORWARDED_FOR", "HTTP_X_FORWARDED_PROTO", "HTTP_VIA", "HTTP_CONNECTION",
        "HTTP_PROXY",
    };
    size_t i;

    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);

        printf("%s=%s\n", names[i], v ? v : "(unset)");
    }
    return 0;
}
