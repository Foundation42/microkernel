#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond)                                                      \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__,   \
                    #cond);                                               \
            return 1;                                                     \
        }                                                                 \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)

#define RUN_TEST(fn)                                                      \
    do {                                                                  \
        tests_run++;                                                      \
        printf("  %-50s", #fn);                                           \
        if (fn() == 0) {                                                  \
            tests_passed++;                                               \
            printf("PASS\n");                                             \
        } else {                                                          \
            printf("FAIL\n");                                             \
        }                                                                 \
    } while (0)

#define TEST_REPORT()                                                     \
    do {                                                                  \
        printf("\n%d/%d tests passed\n", tests_passed, tests_run);        \
        return (tests_passed == tests_run) ? 0 : 1;                       \
    } while (0)

#endif /* TEST_FRAMEWORK_H */
