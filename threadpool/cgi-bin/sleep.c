/*
 * cgi-bin/sleep.c — 예제 CGI: 타임아웃 테스트용. QUERY_STRING "s=<초>" 만큼 잔 뒤 응답 (기본 30초).
 *   GET /cgi-bin/sleep?s=10  → 서버의 CGI 타임아웃이 더 짧으면 504
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    unsigned s = 30;

    if (qs != NULL && strncmp(qs, "s=", 2) == 0)
        s = (unsigned)strtoul(qs + 2, NULL, 10);
    sleep(s);
    printf("Content-Type: text/plain; charset=utf-8\r\n\r\nslept %u\n", s);
    return 0;
}
