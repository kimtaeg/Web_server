/*
 * test_log.c — 레벨 필터, 형식, 다중 스레드에서 줄이 섞이지 않는지.
 */
#include "test.h"

#include <pthread.h>
#include <stdlib.h>

#include "log.h"

#define THREADS 8
#define LINES 300

static void *spam(void *p)
{
    int id = (int)(long)p;
    int i;

    for (i = 0; i < LINES; i++)
        log_msg(LOG_INFO, "T%d-%d BEGIN-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx-END", id, i);
    return NULL;
}

static size_t read_all(FILE *f, char *buf, size_t cap)
{
    size_t n;

    fflush(f);
    rewind(f);
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    return n;
}

static void test_level_filter_and_format(void)
{
    FILE *f = tmpfile();
    char buf[4096];

    REQUIRE(f != NULL);
    log_init(LOG_WARN, f);
    log_msg(LOG_INFO, "hidden");
    log_msg(LOG_WARN, "shown %d", 42);
    log_access("GET", "/index.html?x", 11, 200, 1234, 5);
    read_all(f, buf, sizeof buf);
    CHECK(strstr(buf, "hidden") == NULL);
    CHECK(strstr(buf, "[WARN] shown 42\n") != NULL);
    CHECK(strstr(buf, "GET /index.html") == NULL); /* 접근 로그는 INFO → 걸러짐 */

    log_init(LOG_INFO, f);
    log_access("GET", "/index.html?x", 11, 200, 1234, 5); /* target_len 만큼만 */
    read_all(f, buf, sizeof buf);
    CHECK(strstr(buf, "[INFO] GET /index.html 200 1234B 5ms\n") != NULL);
    log_init(LOG_INFO, NULL);
    fclose(f);
}

static void test_long_target_truncated(void)
{
    FILE *f = tmpfile();
    char target[1000];
    char buf[4096];

    REQUIRE(f != NULL);
    memset(target, 'a', sizeof target);
    log_init(LOG_INFO, f);
    log_access("GET", target, sizeof target, 414, 0, 0);
    read_all(f, buf, sizeof buf);
    CHECK(strstr(buf, "... 414") != NULL);
    CHECK(strlen(buf) < 400);
    log_init(LOG_INFO, NULL);
    fclose(f);
}

/* 여러 스레드가 동시에 써도 각 줄이 온전해야 한다 (mutex 직렬화 검증). */
static void test_concurrent_lines_intact(void)
{
    FILE *f = tmpfile();
    pthread_t th[THREADS];
    char *buf;
    char *line;
    char *save = NULL;
    long i;
    int lines = 0;
    int broken = 0;

    REQUIRE(f != NULL);
    buf = malloc(4 * 1024 * 1024);
    REQUIRE(buf != NULL);
    log_init(LOG_INFO, f);
    for (i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, spam, (void *)i);
    for (i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    read_all(f, buf, 4 * 1024 * 1024);

    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *b = strstr(line, "BEGIN-");
        lines++;
        /* 한 줄에 BEGIN 이 정확히 하나, END 로 끝나야 한다 */
        if (b == NULL || strstr(b + 1, "BEGIN-") != NULL ||
            strcmp(line + strlen(line) - 4, "-END") != 0)
            broken++;
    }
    CHECK_EQ_INT(lines, THREADS * LINES);
    CHECK_EQ_INT(broken, 0);
    log_init(LOG_INFO, NULL);
    fclose(f);
    free(buf);
}

int main(void)
{
    RUN_TEST(test_level_filter_and_format);
    RUN_TEST(test_long_target_truncated);
    RUN_TEST(test_concurrent_lines_intact);
    TEST_END();
}
