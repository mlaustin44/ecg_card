/* Minimal unit-test harness. One executable per test file:
 *
 *   TEST(name) { CHECK(x == 1); CHECK_NEAR(a, b, tol); }
 *   int main(void) { RUN(name); return TEST_RESULT(); }
 *
 * Tests run with the repository root as the working directory. */
#ifndef ECG_TEST_H
#define ECG_TEST_H

#include <math.h>
#include <stdio.h>

static int test_failures_;
static int test_checks_;

#define TEST(name) static void test_##name(void)
#define RUN(name)                                   \
    do {                                            \
        int before_ = test_failures_;               \
        test_##name();                              \
        printf("%s %s\n", test_failures_ == before_ ? "ok  " : "FAIL", #name); \
    } while (0)

#define CHECK(cond)                                                          \
    do {                                                                     \
        test_checks_++;                                                      \
        if (!(cond)) {                                                       \
            test_failures_++;                                                \
            printf("  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);       \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long a_ = (long long)(a), b_ = (long long)(b);                  \
        test_checks_++;                                                      \
        if (a_ != b_) {                                                      \
            test_failures_++;                                                \
            printf("  %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                   #a, #b, a_, b_);                                          \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                \
    do {                                                                     \
        double a_ = (double)(a), b_ = (double)(b), t_ = (double)(tol);       \
        test_checks_++;                                                      \
        if (!(fabs(a_ - b_) <= t_)) {                                        \
            test_failures_++;                                                \
            printf("  %s:%d: %s ~= %s (%g vs %g, tol %g)\n", __FILE__,       \
                   __LINE__, #a, #b, a_, b_, t_);                            \
        }                                                                    \
    } while (0)

#define TEST_RESULT()                                                        \
    (printf("%d checks, %d failures\n", test_checks_, test_failures_),       \
     test_failures_ ? 1 : 0)

#endif
