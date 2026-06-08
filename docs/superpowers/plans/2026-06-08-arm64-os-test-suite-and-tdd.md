# MyOSv2 — Test Suite + TDD Enforcement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an in-kernel test framework + tests that run on every boot (halt on fail), a `make test` that returns a shell exit code via ARM semihosting, and a pre-commit hook that blocks red commits.

**Architecture:** `ktest.*` is a tiny framework (KASSERT + a runner). `tests.*` holds characterization tests for pmm/kheap and `run_self_tests()`. `semihost.*` exits QEMU from the guest. `kmain` runs the suite after `mmu_init`; a `-DTEST_EXIT` build exits with a code, a normal build halts on failure. `.githooks/pre-commit` runs `make test` and blocks the commit on failure.

**Tech Stack:** Freestanding C, ARM semihosting (`hlt #0xF000`), QEMU `virt`, GNU make, git hooks.

**Note:** No host test runner — the suite runs in QEMU. Verifications use `make run` (observe PASS lines) and `make test` (check `echo $?`). All new code is heavily commented (project preference).

---

## File structure

| File | Responsibility |
|------|----------------|
| `src/ktest.h` / `ktest.c` | KASSERT macro + the test runner |
| `src/tests.h` / `tests.c` | Characterization tests + `run_self_tests()` |
| `src/semihost.h` / `semihost.c` | `qemu_exit(code)` via ARM semihosting |
| `src/kmain.c` | (modified) run tests after `mmu_init`; halt-on-fail / exit in test mode |
| `Makefile` | (modified) `EXTRA_CFLAGS` + `test` target |
| `.githooks/pre-commit` | (new) run `make test`; block commit on failure |
| `docs/tdd-workflow.md` | (new) the red→green→refactor workflow |

---

## Task 1: KTEST framework

**Files:**
- Create: `src/ktest.h`
- Create: `src/ktest.c`

- [ ] **Step 1: Write `src/ktest.h`**

```c
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
```

- [ ] **Step 2: Write `src/ktest.c`**

```c
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
```

- [ ] **Step 3: Verify it builds**

Run:
```bash
make clean && make
```
Expected: builds with no errors (only the benign RWX warning). Not wired in yet.

- [ ] **Step 4: Commit**

```bash
git add src/ktest.h src/ktest.c
git commit -m "feat: add minimal in-kernel test framework (KASSERT + runner)"
```

---

## Task 2: Tests for pmm/kheap + boot integration

**Files:**
- Create: `src/tests.h`
- Create: `src/tests.c`
- Modify: `src/kmain.c`

- [ ] **Step 1: Write `src/tests.h`**

```c
// tests.h -- the MyOSv2 self-test suite entry point.
#pragma once

// Run every self-test. Returns the number of FAILED tests (0 == all passed).
int run_self_tests(void);
```

- [ ] **Step 2: Write `src/tests.c`**

```c
// tests.c -- characterization tests for the existing kernel subsystems.
// =====================================================================
//
// These lock in the behavior we verified by hand in Phase 4 so a future change
// can't silently break it. Each test re-initializes the allocators first so the
// tests are order-independent; re-init just resets bookkeeping pointers (any
// "leaked" pages are irrelevant -- there are ~244 MiB free).
//
// Going forward (Phase 5+) we work test-FIRST: add a failing KASSERT here, watch
// `make test` go red, then implement until green.

#include <stdint.h>
#include "tests.h"
#include "ktest.h"
#include "pmm.h"
#include "kheap.h"

#define PAGE 0x1000UL

// --- PMM ---

static void test_pmm_aligned_and_contiguous(void)
{
    pmm_init();
    void *p1 = pmm_alloc();
    void *p2 = pmm_alloc();
    void *p3 = pmm_alloc();
    KASSERT(p1 != 0);
    KASSERT(((uint64_t)p1 & 0xFFF) == 0);             // page-aligned
    KASSERT((uint64_t)p2 - (uint64_t)p1 == PAGE);     // consecutive, 4 KiB apart
    KASSERT((uint64_t)p3 - (uint64_t)p2 == PAGE);
}

static void test_pmm_free_reuse(void)
{
    pmm_init();
    void *a = pmm_alloc();
    void *b = pmm_alloc();
    pmm_free(b);
    void *c = pmm_alloc();
    KASSERT(c == b);     // the freed page is handed back
    KASSERT(a != b);
}

static void test_pmm_alloc_pages_contiguous(void)
{
    pmm_init();
    void *run = pmm_alloc_pages(3);
    KASSERT(run != 0);
    KASSERT(((uint64_t)run & 0xFFF) == 0);
    void *next = pmm_alloc();
    KASSERT((uint64_t)next - (uint64_t)run == 3 * PAGE);   // run really was 3 pages
}

// --- Heap ---

static void test_kheap_write_read(void)
{
    pmm_init();
    kheap_init();
    int *p = kmalloc(sizeof(int) * 4);
    KASSERT(p != 0);
    KASSERT(((uint64_t)p & 0x7) == 0);     // 8-byte aligned
    for (int i = 0; i < 4; i++) {
        p[i] = i * 11;
    }
    KASSERT(p[0] == 0 && p[1] == 11 && p[2] == 22 && p[3] == 33);
}

static void test_kheap_free_reuse(void)
{
    pmm_init();
    kheap_init();
    void *a = kmalloc(64);
    kfree(a);
    void *b = kmalloc(64);
    KASSERT(b == a);     // a same-size request reuses the freed block
}

static void test_kheap_coalesce(void)
{
    pmm_init();
    kheap_init();
    char *a = kmalloc(32);
    char *b = kmalloc(64);
    char *c = kmalloc(16);
    (void)c;
    kfree(b);
    kfree(a);                  // merges a + b into one chunk
    char *big = kmalloc(80);   // too big for a(32) or b(64) alone
    KASSERT(big == a);         // only fits because the two coalesced
}

// The registry of all tests.
static const struct ktest tests[] = {
    { "pmm: pages aligned & contiguous", test_pmm_aligned_and_contiguous },
    { "pmm: freed page reused",          test_pmm_free_reuse },
    { "pmm: alloc_pages contiguous run", test_pmm_alloc_pages_contiguous },
    { "kheap: alloc write/read",         test_kheap_write_read },
    { "kheap: freed block reused",       test_kheap_free_reuse },
    { "kheap: coalesce adjacent blocks", test_kheap_coalesce },
};

int run_self_tests(void)
{
    return ktest_run(tests, sizeof(tests) / sizeof(tests[0]));
}
```

- [ ] **Step 3: Wire the suite into `src/kmain.c`**

Add `#include "tests.h"` with the other includes (after `#include "kheap.h"`):

```c
#include "tests.h"
```

Then insert the self-test phase immediately AFTER the `kprintf("MMU enabled.\n");`
line and BEFORE `pmm_init();`:

```c
    // --- Self-tests: verify the foundations before doing anything else. ---
    // On a normal build, a failure halts the kernel (don't limp forward broken).
    // The `make test` build (-DTEST_EXIT) instead exits QEMU with a status code.
    int failed = run_self_tests();
#ifndef TEST_EXIT
    if (failed) {
        kprintf("SELF-TESTS FAILED -- halting.\n");
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
#endif
```

(`TEST_EXIT` handling that actually exits QEMU is added in Task 3, which also adds
the `#include "semihost.h"`.)

- [ ] **Step 4: Build and run — watch the tests pass**

Run:
```bash
make clean && make run
```
Expected (then the demos and ticks — quit with `Ctrl-C`):
```
MMU enabled.
==== self-tests: running 6 ====
[PASS] pmm: pages aligned & contiguous
[PASS] pmm: freed page reused
[PASS] pmm: alloc_pages contiguous run
[PASS] kheap: alloc write/read
[PASS] kheap: freed block reused
[PASS] kheap: coalesce adjacent blocks
==== self-tests: 6 passed, 0 failed ====
PMM: three pages -> ...
...
tick 1
```

- [ ] **Step 5: Commit**

```bash
git add src/tests.h src/tests.c src/kmain.c
git commit -m "test: add pmm/kheap self-tests; run them at boot (halt on fail)"
```

---

## Task 3: `qemu_exit` via semihosting + `make test`

**Files:**
- Create: `src/semihost.h`
- Create: `src/semihost.c`
- Modify: `src/kmain.c`
- Modify: `Makefile`

- [ ] **Step 1: Write `src/semihost.h`**

```c
// semihost.h -- talk to the host (QEMU) via ARM semihosting.
#pragma once

// Terminate QEMU with the given process exit code (used by `make test`).
void qemu_exit(int code);
```

- [ ] **Step 2: Write `src/semihost.c`**

```c
// semihost.c -- ARM semihosting: ask QEMU to exit with a status code.
// ===================================================================
//
// "Semihosting" is a debug channel where the guest asks the host (here QEMU) to
// do something on its behalf -- file I/O, console, or EXIT. The guest puts an
// operation number in x0, a parameter (block) pointer in x1, and executes the
// AArch64 semihosting trap `HLT #0xF000`. QEMU (run with -semihosting) handles
// it. We only use SYS_EXIT, to end a test run with a pass/fail code.

#include <stdint.h>
#include "semihost.h"

#define SYS_EXIT 0x18                       // semihosting "exit" operation
#define ADP_Stopped_ApplicationExit 0x20026 // "the application exited normally"

void qemu_exit(int code)
{
    // On AArch64, SYS_EXIT takes a two-word block: { reason, exit_status }.
    // QEMU exits its own process with exit_status as the code.
    uint64_t block[2] = { ADP_Stopped_ApplicationExit,
                          (uint64_t)(unsigned int)code };

    register uint64_t x0 __asm__("x0") = SYS_EXIT;
    register uint64_t x1 __asm__("x1") = (uint64_t)block;
    __asm__ volatile("hlt #0xF000" : : "r"(x0), "r"(x1) : "memory");

    // Only reached if semihosting is disabled (HLT would otherwise fault). We
    // only call this in the -semihosting test build, so just halt defensively.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
```

- [ ] **Step 3: Use `qemu_exit` in `src/kmain.c`**

Add the include next to `#include "tests.h"`:

```c
#include "semihost.h"
```

Then change the self-test phase so the `TEST_EXIT` build exits with a code.
Replace the block added in Task 2 with:

```c
    // --- Self-tests: verify the foundations before doing anything else. ---
    int failed = run_self_tests();
#ifdef TEST_EXIT
    // `make test` build: report the result to the shell and stop.
    qemu_exit(failed == 0 ? 0 : 1);
#else
    // Normal build: a failure is fatal -- halt rather than run broken.
    if (failed) {
        kprintf("SELF-TESTS FAILED -- halting.\n");
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
#endif
```

- [ ] **Step 4: Add `EXTRA_CFLAGS` and the `test` target to the `Makefile`**

Append `$(EXTRA_CFLAGS)` to the `CFLAGS` definition:

```make
CFLAGS  := -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only \
           -Wall -Wextra -O2 -g -ffunction-sections -MMD -MP $(EXTRA_CFLAGS)
```

Add `test` to the `.PHONY` line:

```make
.PHONY: all run debug gdb clean objdump compile_commands test
```

Add this target (place it after the `run` target):

```make
# Run the self-tests and return a shell exit code (0 = all passed). Builds a
# test kernel with -DTEST_EXIT (which exits QEMU via semihosting), runs it under
# -semihosting, then cleans so the flag never leaks into a normal `make run`.
test:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory EXTRA_CFLAGS=-DTEST_EXIT $(TARGET)
	@echo "--- running self-tests in QEMU ---"
	@$(QEMU) $(QEMU_FLAGS) -semihosting; status=$$?; \
	  $(MAKE) --no-print-directory clean >/dev/null; \
	  echo "make test exit code: $$status"; \
	  exit $$status
```

- [ ] **Step 5: Verify the green path returns exit code 0**

Run:
```bash
make test; echo "shell saw: $?"
```
Expected: the `[PASS]` lines, `==== self-tests: 6 passed, 0 failed ====`, then
`make test exit code: 0` and `shell saw: 0`.

- [ ] **Step 6: Verify the RED path returns non-zero (then restore)**

Temporarily break one assertion to prove failures propagate:
```bash
sed -i '' 's/KASSERT(c == b);/KASSERT(c == (void*)1);/' src/tests.c
make test; echo "shell saw: $?"
```
Expected: a `[FAIL] pmm: freed page reused` line, `1 failed`, and
`shell saw: 1` (non-zero). Now restore:
```bash
git checkout src/tests.c
make test; echo "shell saw: $?"
```
Expected: back to all pass, `shell saw: 0`.

- [ ] **Step 7: Commit**

```bash
git add src/semihost.h src/semihost.c src/kmain.c Makefile
git commit -m "feat: make test -> QEMU semihosting exit code (green/red automation)"
```

---

## Task 4: Pre-commit hook (green-bar gate)

**Files:**
- Create: `.githooks/pre-commit`

- [ ] **Step 1: Write `.githooks/pre-commit`**

```sh
#!/bin/sh
# Green-bar TDD gate: refuse to commit while the self-test suite is red.
# Enabled via:  git config core.hooksPath .githooks
echo "pre-commit: running self-tests (make test)..."
if ! make test; then
    echo
    echo "pre-commit: TESTS FAILED -- commit aborted."
    echo "Fix the suite ('make test' until green), then commit again."
    exit 1
fi
echo "pre-commit: all tests passed; allowing commit."
```

- [ ] **Step 2: Make it executable and point git at it**

Run:
```bash
chmod +x .githooks/pre-commit
git config core.hooksPath .githooks
echo "hooksPath = $(git config core.hooksPath)"
```
Expected: `hooksPath = .githooks`.

- [ ] **Step 3: Prove the gate BLOCKS a red commit**

Break a test, stage it, and attempt a commit:
```bash
sed -i '' 's/KASSERT(b == a);/KASSERT(b == (void*)1);/' src/tests.c
git add src/tests.c
git commit -m "should be blocked"
echo "commit exit code: $?"
```
Expected: the hook runs, tests fail, the commit is **aborted** with a non-zero
exit code, and `git log` does NOT show "should be blocked".

- [ ] **Step 4: Restore and prove the gate ALLOWS a green commit**

```bash
git checkout src/tests.c
git add .githooks/pre-commit
git commit -m "feat: add pre-commit hook that blocks red commits (TDD gate)"
echo "commit exit code: $?"
```
Expected: the hook runs, all tests pass, and the commit succeeds (exit code 0).

---

## Task 5: TDD workflow doc + notes

**Files:**
- Create: `docs/tdd-workflow.md`
- Create: `docs/notes/testing.md`

- [ ] **Step 1: Write `docs/tdd-workflow.md`**

```markdown
# MyOSv2 TDD workflow

MyOSv2 is developed test-first. The loop:

1. **RED** — add a failing test in `src/tests.c` (a new `test_*` function plus an
   entry in the `tests[]` array). Run `make test`; watch it fail.
2. **GREEN** — write the minimum code to make it pass. Run `make test`; watch it pass.
3. **REFACTOR** — clean up with the test still green.

## Running the tests

- `make run` — boots normally; the suite runs first and the kernel HALTS if any
  test fails (so you never demo a broken build).
- `make test` — builds a test kernel, runs the suite in QEMU, and returns a shell
  exit code (0 = all passed). Use this in scripts/CI.

## The commit gate

A pre-commit hook (`.githooks/pre-commit`, enabled with
`git config core.hooksPath .githooks`) runs `make test` and blocks the commit if
anything is red. Do not bypass it with `--no-verify`.

## Adding a test

```c
static void test_my_thing(void)
{
    KASSERT(some_condition);
}
// ...then add to the tests[] array:
{ "my thing", test_my_thing },
```

New contributors must run `git config core.hooksPath .githooks` once after cloning
to activate the gate.
```

- [ ] **Step 2: Write `docs/notes/testing.md`**

Cover: on-target vs host testing and why this kernel tests on-target; how the
KTEST framework tracks per-test failure; what ARM semihosting is and how
`qemu_exit` turns a test result into a QEMU process exit code; why `make test`
double-cleans (so `-DTEST_EXIT` never leaks into `make run`); and the pre-commit
gate. Note the known limitation that `kprintf` output formatting isn't yet tested.

- [ ] **Step 3: Commit**

```bash
git add docs/tdd-workflow.md docs/notes/testing.md
git commit -m "docs: TDD workflow + testing notes"
```

---

## Self-review

- **Spec coverage:** KTEST framework + KASSERT + runner (T1), pmm/kheap tests + `run_self_tests` + boot integration with halt-on-fail (T2), `qemu_exit` semihosting + `make test` + `-DTEST_EXIT` exit path (T3), green path exit 0 and red path non-zero verified (T3 steps 5-6), pre-commit hook blocking red / allowing green (T4), workflow doc + notes (T5). All success criteria exercised.
- **Placeholder scan:** none — all code/commands complete. The `sed` break/restore steps are deliberate verifications, immediately reverted with `git checkout`.
- **Type consistency:** `KASSERT`/`ktest__assert`/`ktest_run`/`struct ktest` consistent across `ktest.h` (T1) and callers in `tests.c` (T2). `run_self_tests` declared in `tests.h` (T2), called in `kmain.c` (T2/T3). `qemu_exit(int)` declared in `semihost.h` (T3), defined in `semihost.c` (T3), called in `kmain.c` (T3). `EXTRA_CFLAGS`/`TEST_EXIT` consistent between the Makefile `test` target and the `#ifdef` in `kmain.c` (T3).
