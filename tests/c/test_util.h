/* Tiny assertion helpers shared by the C unit tests. Deliberately
 * minimal -- no framework to install, no build system, just a header and
 * a `main` per test file (see tests/c/README or run_c_tests.sh). */
#ifndef CAPTURE2CLOUD_TEST_UTIL_H
#define CAPTURE2CLOUD_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int t_passed = 0;
static int t_failed = 0;
static const char *t_group = "";

__attribute__((unused)) static void t_begin(const char *name) {
    t_group = name;
    printf("\n%s\n", name);
}

__attribute__((unused)) static void t_ok(const char *what, int condition) {
    if (condition) {
        t_passed++;
    } else {
        t_failed++;
        printf("  FAIL %s\n", what);
    }
}

__attribute__((unused)) static void t_eq_int(const char *what, long long actual, long long expected) {
    if (actual == expected) {
        t_passed++;
    } else {
        t_failed++;
        printf("  FAIL %s: expected %lld, got %lld\n", what, expected, actual);
    }
}

/* Not every test file uses every helper; the attribute keeps -Wall
 * quiet about the ones a given file happens not to need. */
__attribute__((unused)) static void t_eq_str(const char *what, const char *actual, const char *expected) {
    if (actual && expected && strcmp(actual, expected) == 0) {
        t_passed++;
    } else {
        t_failed++;
        printf("  FAIL %s: expected \"%s\", got \"%s\"\n", what, expected ? expected : "(null)",
               actual ? actual : "(null)");
    }
}

static int t_report(void) {
    (void)t_group;
    printf("\n%d passed, %d failed\n", t_passed, t_failed);
    return t_failed == 0 ? 0 : 1;
}

#endif
