// ktest.c -- the test runner behind ktest.h.
#include "ktest.h"
#include "kprintf.h"

// Set to 1 if any KASSERT fails during the currently-running test. Reset before
// each test so failures are attributed to the right one.
static int current_failed;

void ktest__assert(int ok, const char *expr, const char *file, int line)
{
    if (!ok) {
        current_failed = 1;
        kprintf("    assertion failed: %s  (%s:%d)\n", expr, file, line);
    }
}

int ktest_run(const struct ktest *tests, int count)
{
    int failed = 0;
    kprintf("==== self-tests: running %d ====\n", count);
    for (int i = 0; i < count; i++) {
        current_failed = 0;       // start each test clean
        tests[i].fn();            // run it (its KASSERTs may set current_failed)
        if (current_failed) {
            kprintf("[FAIL] %s\n", tests[i].name);
            failed++;
        } else {
            kprintf("[PASS] %s\n", tests[i].name);
        }
    }
    kprintf("==== self-tests: %d passed, %d failed ====\n",
            count - failed, failed);
    return failed;
}
