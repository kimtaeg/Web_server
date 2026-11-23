#include <stdio.h>

#include "test.h"

int t_checks, t_failures;

static void run(const char *name, void (*suite)(void)) {
    int before_checks = t_checks, before_fail = t_failures;
    suite();
    printf("  %-11s %4d checks  %s\n", name, t_checks - before_checks,
           t_failures == before_fail ? "ok" : "FAILED");
}

int main(void) {
    printf("unit tests\n");
    run("http", suite_http);
    run("mime", suite_mime);
    run("router", suite_router);
    run("cache", suite_cache);
    run("config", suite_config);
    run("static", suite_static);
    run("proxy", suite_proxy);
    run("cgi", suite_cgi);
    run("threadpool", suite_threadpool);
    printf("%d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
