/*
 * cgi-bin/adder.c — 예제 CGI: QUERY_STRING 의 두 정수 a, b 의 합.
 *   GET /cgi-bin/adder?a=3&b=4  →  "7"
 * 인자가 없거나 정수가 아니거나 범위를 넘으면 Status: 400.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "a=3&b=4" 에서 key 의 값을 찾는다. 성공 0. */
static int get_ll(const char *qs, const char *key, long long *out)
{
    size_t kl = strlen(key);
    const char *p = qs;

    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            char *end;
            const char *v = p + kl + 1;

            errno = 0;
            *out = strtoll(v, &end, 10);
            if (end == v || (*end != '\0' && *end != '&') || errno == ERANGE)
                return -1;
            return 0;
        }
        p = strchr(p, '&');
        if (p != NULL)
            p++;
    }
    return -1;
}

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    long long a, b;

    if (qs == NULL || get_ll(qs, "a", &a) != 0 || get_ll(qs, "b", &b) != 0) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
               "usage: /cgi-bin/adder?a=<정수>&b=<정수>\n");
        return 0;
    }
    if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
               "overflow\n");
        return 0;
    }
    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n%lld\n", a + b);
    return 0;
}
