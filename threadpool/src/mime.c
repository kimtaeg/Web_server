/*
 * mime.c — 확장자 → Content-Type. 설계 이유는 mime.h 참고.
 *
 * 동기화: 테이블이 const 이고 함수가 상태를 갖지 않으므로 락이 필요 없다.
 */
#include "mime.h"

#include <stddef.h>
#include <string.h>
#include <strings.h> /* strcasecmp (POSIX) */

#define UTF8 "; charset=utf-8"

static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    /* 텍스트: 브라우저가 인코딩을 추측하지 않도록 charset 명시 */
    {"html", "text/html" UTF8},
    {"htm", "text/html" UTF8},
    {"css", "text/css" UTF8},
    {"js", "text/javascript" UTF8},
    {"mjs", "text/javascript" UTF8},
    {"txt", "text/plain" UTF8},
    {"csv", "text/csv" UTF8},
    {"md", "text/markdown" UTF8},
    {"json", "application/json"},
    {"xml", "application/xml"},
    /* 이미지 */
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    /* 폰트 */
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    /* 미디어 */
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mp3", "audio/mpeg"},
    {"wav", "audio/wav"},
    /* 기타 */
    {"pdf", "application/pdf"},
    {"wasm", "application/wasm"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"tar", "application/x-tar"},
};

const char *mime_type_for_path(const char *path)
{
    const char *base;
    const char *dot;
    size_t i;

    if (path == NULL)
        return MIME_DEFAULT;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    /* 점이 없음 / 맨 앞(숨김 파일) / 맨 끝(빈 확장자) → 기본값 */
    if (dot == NULL || dot == base || dot[1] == '\0')
        return MIME_DEFAULT;
    for (i = 0; i < sizeof mime_table / sizeof mime_table[0]; i++)
        if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
            return mime_table[i].type;
    return MIME_DEFAULT;
}
