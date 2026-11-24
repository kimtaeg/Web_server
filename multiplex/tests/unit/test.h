/*
 * test.h — 외부 라이브러리 없는 최소 단위 테스트 매크로.
 *
 * 사용법:
 *   static void test_foo(void) { CHECK(1 + 1 == 2); CHECK_EQ_INT(f(), 3); }
 *   int main(void) { RUN_TEST(test_foo); TEST_END(); }
 *
 * - CHECK 는 실패해도 테스트를 중단하지 않는다 (한 번에 여러 실패를 본다).
 *   이어서 진행하면 위험한 경우(NULL 역참조 등)에는 REQUIRE 를 쓴다: 실패 시 해당 테스트 함수를 return.
 * - 실패가 하나라도 있으면 프로세스 종료 코드가 1 → tests/run_unit.sh 와 make 가 실패로 인식.
 * - 각 테스트 파일은 독립 실행 파일이므로 static 카운터를 헤더에 둬도 충돌하지 않는다.
 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int test_checks_run;
static int test_checks_failed;
static int test_funcs_run;
static int test_funcs_failed;
static const char *test_current = "";

#define TEST_FAIL_MSG_(fmt, ...)                                                       \
    do {                                                                               \
        test_checks_failed++;                                                          \
        fprintf(stderr, "    FAIL %s:%d [%s] " fmt "\n", __FILE__, __LINE__, test_current, \
                __VA_ARGS__);                                                          \
    } while (0)

#define CHECK(cond)                                         \
    do {                                                    \
        test_checks_run++;                                  \
        if (!(cond))                                        \
            TEST_FAIL_MSG_("CHECK(%s)", #cond);             \
    } while (0)

#define REQUIRE(cond)                                       \
    do {                                                    \
        test_checks_run++;                                  \
        if (!(cond)) {                                      \
            TEST_FAIL_MSG_("REQUIRE(%s)", #cond);           \
            return;                                         \
        }                                                   \
    } while (0)

/* 부호 있는 정수 (int, ssize_t, enum). */
#define CHECK_EQ_INT(a, b)                                                       \
    do {                                                                         \
        long long a_ = (long long)(a);                                           \
        long long b_ = (long long)(b);                                           \
        test_checks_run++;                                                       \
        if (a_ != b_)                                                            \
            TEST_FAIL_MSG_("%s == %s (%lld != %lld)", #a, #b, a_, b_);           \
    } while (0)

/* 부호 없는 크기 (size_t). */
#define CHECK_EQ_SIZE(a, b)                                                      \
    do {                                                                         \
        unsigned long long a_ = (unsigned long long)(a);                         \
        unsigned long long b_ = (unsigned long long)(b);                         \
        test_checks_run++;                                                       \
        if (a_ != b_)                                                            \
            TEST_FAIL_MSG_("%s == %s (%llu != %llu)", #a, #b, a_, b_);           \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                       \
    do {                                                                         \
        const char *a_ = (a);                                                    \
        const char *b_ = (b);                                                    \
        test_checks_run++;                                                       \
        if (a_ == NULL || b_ == NULL || strcmp(a_, b_) != 0)                     \
            TEST_FAIL_MSG_("%s == %s (\"%s\" != \"%s\")", #a, #b,                \
                           a_ ? a_ : "(null)", b_ ? b_ : "(null)");              \
    } while (0)

/* 길이 n 의 바이트 비교 (NUL 이 섞인 데이터용). */
#define CHECK_EQ_MEM(a, b, n)                                                    \
    do {                                                                         \
        test_checks_run++;                                                       \
        if (memcmp((a), (b), (n)) != 0)                                          \
            TEST_FAIL_MSG_("memcmp(%s, %s, %s) != 0", #a, #b, #n);               \
    } while (0)

#define RUN_TEST(fn)                                                             \
    do {                                                                         \
        int before_ = test_checks_failed;                                        \
        test_current = #fn;                                                      \
        fn();                                                                    \
        test_funcs_run++;                                                        \
        if (test_checks_failed != before_)                                       \
            test_funcs_failed++;                                                 \
        printf("  %s %s\n", test_checks_failed == before_ ? "ok  " : "FAIL", #fn); \
    } while (0)

/* main 의 마지막 문장으로 쓴다. */
#define TEST_END()                                                               \
    do {                                                                         \
        printf("  -- %s: %d/%d tests, %d/%d checks passed\n", __FILE__,          \
               test_funcs_run - test_funcs_failed, test_funcs_run,               \
               test_checks_run - test_checks_failed, test_checks_run);           \
        return test_checks_failed ? 1 : 0;                                       \
    } while (0)

#endif /* TEST_H */
