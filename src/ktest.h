// ktest.h -- a minimal in-kernel test framework.
// ==============================================
//
// A test is just a void(void) function that makes assertions with KASSERT.
// ktest_run() runs an array of them, prints [PASS]/[FAIL] per test plus a
// summary, and returns how many tests failed. There is no host test runner --
// these run inside the kernel under QEMU, the only place the hardware behaves
// like the real thing.
#pragma once

// One test case: a human-readable name and the function that performs it.
struct ktest {
    const char *name;
    void (*fn)(void);
};

// Record one assertion result. On failure it marks the running test as failed
// and prints the stringified expression and its location. Called via KASSERT.
void ktest__assert(int ok, const char *expr, const char *file, int line);

// Assert that `cond` is true. #cond turns the expression into a string so a
// failure report shows exactly what was checked.
#define KASSERT(cond) ktest__assert((cond) ? 1 : 0, #cond, __FILE__, __LINE__)

// Run all `count` tests; print results; return the number that FAILED.
int ktest_run(const struct ktest *tests, int count);
