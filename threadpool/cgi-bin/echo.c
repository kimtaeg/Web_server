/*
 * cgi-bin/echo.c — 예제 CGI: POST 본문(stdin)을 그대로 돌려준다.
 * Content-Type 은 요청의 CONTENT_TYPE 을 그대로 쓰고, 없으면 application/octet-stream.
 * stdin 은 CONTENT_LENGTH 만큼만 읽는다 (CGI/1.1 규약: EOF 를 기다리지 말 것).
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    const char *cl = getenv("CONTENT_LENGTH");
    const char *ct = getenv("CONTENT_TYPE");
    unsigned long long left = cl ? strtoull(cl, NULL, 10) : 0;
    char buf[8192];

    printf("Content-Type: %s\r\n\r\n", ct && *ct ? ct : "application/octet-stream");
    fflush(stdout);
    while (left > 0) {
        size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
        ssize_t r = read(STDIN_FILENO, buf, want);
        ssize_t off = 0;

        if (r <= 0)
            break;
        while (off < r) {
            ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(r - off));

            if (w <= 0)
                return 1;
            off += w;
        }
        left -= (unsigned long long)r;
    }
    return 0;
}
