// test_util.h: tiny assert harness shared by the test binaries.
#ifndef DP_TEST_UTIL_H
#define DP_TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>

static int dp_fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
            dp_fails++;                                                                          \
        }                                                                                        \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                     \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg);            \
            dp_fails++;                                                                          \
        }                                                                                        \
    } while (0)

#define TEST_DONE(name)                                                                          \
    do {                                                                                         \
        if (dp_fails) {                                                                          \
            fprintf(stderr, "%s: %d failure(s)\n", name, dp_fails);                              \
            return 1;                                                                            \
        }                                                                                        \
        printf("%s: ok\n", name);                                                                \
        return 0;                                                                                \
    } while (0)
#endif
