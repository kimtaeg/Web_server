/*
 * test_mime.c — mime 모듈 단위 테스트 (테이블 기반).
 */
#include "test.h"

#include "mime.h"

#define HTML "text/html; charset=utf-8"

static const struct {
    const char *path;
    const char *want;
} cases[] = {
    {"/index.html", HTML},
    {"/a/b/page.htm", HTML},
    {"/INDEX.HTML", HTML}, /* 대소문자 무시 */
    {"/x.Css", "text/css; charset=utf-8"},
    {"/app.js", "text/javascript; charset=utf-8"},
    {"/app.mjs", "text/javascript; charset=utf-8"},
    {"/d.json", "application/json"},
    {"/r.txt", "text/plain; charset=utf-8"},
    {"/i.png", "image/png"},
    {"/i.jpg", "image/jpeg"},
    {"/i.JPEG", "image/jpeg"},
    {"/i.gif", "image/gif"},
    {"/i.svg", "image/svg+xml"},
    {"/favicon.ico", "image/x-icon"},
    {"/i.webp", "image/webp"},
    {"/doc.pdf", "application/pdf"},
    {"/m.wasm", "application/wasm"},
    {"/v.mp4", "video/mp4"},
    {"/f.woff2", "font/woff2"},
    {"/archive.tar.gz", "application/gzip"}, /* 마지막 확장자 기준 */
    {"relative.css", "text/css; charset=utf-8"},
    /* 기본값으로 떨어져야 하는 경우 */
    {"/README", MIME_DEFAULT},        /* 확장자 없음 */
    {"/.bashrc", MIME_DEFAULT},       /* 숨김 파일: 점이 맨 앞 */
    {"/dir.d/file", MIME_DEFAULT},    /* 점이 디렉터리 이름에만 */
    {"/file.", MIME_DEFAULT},         /* 빈 확장자 */
    {"/file.unknownext", MIME_DEFAULT},
    {"/file.htmlx", MIME_DEFAULT},    /* 접두사 일치만으로는 안 됨 */
    {"/file.ht", MIME_DEFAULT},
    {"", MIME_DEFAULT},
    {"/", MIME_DEFAULT},
};

static void test_table(void)
{
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK_EQ_STR(mime_type_for_path(cases[i].path), cases[i].want);
}

static void test_null(void)
{
    CHECK_EQ_STR(mime_type_for_path(NULL), MIME_DEFAULT);
}

int main(void)
{
    RUN_TEST(test_table);
    RUN_TEST(test_null);
    TEST_END();
}
