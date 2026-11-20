/*
 * cgi-bin/cachectl.c — 예제 CGI: 캐시 동작 테스트용.
 *   ?cc=<값>     → "Cache-Control: <값>" 헤더 (예: cc=no-store, cc=private, cc=max-age=1)
 *   ?cookie=1    → Set-Cookie 헤더
 *   ?size=<N>    → 본문을 N 바이트로 채움 (객체 크기 상한 테스트)
 * 본문 첫 줄은 매 실행마다 다른 토큰(pid + 나노초) → 같은 본문이면 캐시에서 온 것.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* "a=1&b=2" 에서 key 의 값을 out 으로 ('&' 전까지). 없으면 0 */
static int qget(const char *qs, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = qs;

    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t len = strcspn(v, "&");

            if (len >= n)
                len = n - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return 0;
}

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    char cc[128], tmp[32];
    struct timespec ts;
    long size = 0;
    char token[64];
    int tl;

    if (qs == NULL)
        qs = "";
    clock_gettime(CLOCK_REALTIME, &ts);
    tl = snprintf(token, sizeof token, "token-%ld-%ld\n", (long)getpid(), (long)ts.tv_nsec);
    if (qget(qs, "size", tmp, sizeof tmp))
        size = strtol(tmp, NULL, 10);

    printf("Content-Type: text/plain\r\n");
    if (qget(qs, "cc", cc, sizeof cc))
        printf("Cache-Control: %s\r\n", cc);
    if (qget(qs, "cookie", tmp, sizeof tmp))
        printf("Set-Cookie: session=secret\r\n");
    printf("\r\n%s", token);
    for (; size > tl; size--)
        putchar('x');
    return 0;
}
