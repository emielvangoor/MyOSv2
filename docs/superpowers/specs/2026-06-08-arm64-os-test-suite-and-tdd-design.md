# MyOSv2 — Test Suite + TDD Enforcement Design

**Date:** 2026-06-08
**Status:** Approved

## Goal

Give MyOSv2 an on-target test suite that runs at every boot, a CI-style exit
code for automation, and mechanical + workflow enforcement of test-driven
development.

Two halves:
1. **Test suite** — an in-kernel framework + tests for the existing code, run on
   every boot (halt on failure), plus `make test` returning a shell exit code.
2. **TDD enforcement** — a pre-commit hook that blocks red commits, plus a
   documented red→green→refactor workflow used for all future work.

## Part 1 — Test suite

### Architecture

```
ktest.h/.c    framework: KASSERT macro + a runner printing [PASS]/[FAIL]
tests.h/.c    the test cases for pmm + kheap, and run_self_tests()
semihost.h/.c qemu_exit(code): exit QEMU from inside the kernel (ARM semihosting)
kmain.c       run the suite right after mmu_init; halt on fail / exit in test mode
```

Boot flow:
```
uart_init -> mmu_init -> [ self-tests ] -> demos -> interrupts -> ticks
                              |
                all pass -----+----- any fail -> print failures, HALT (wfi loop)
```

### KTEST framework (`ktest.h` / `ktest.c`)

- A test is a `void fn(void)` that makes assertions.
- `#define KASSERT(cond) ktest__assert((cond) ? 1 : 0, #cond, __FILE__, __LINE__)`
  — `#cond` stringifies the expression so failures print exactly what was checked.
- `void ktest__assert(int ok, const char *expr, const char *file, int line)` —
  on failure, marks the current test failed and prints `expr` + `file:line`.
- `int ktest_run(const struct ktest *tests, int count)` — runs each test,
  prints `[PASS] name` / `[FAIL] name`, prints a summary, returns the number of
  failed tests.
- `struct ktest { const char *name; void (*fn)(void); };`

### Tests (`tests.c`)

Each test re-inits the allocators first (`pmm_init`/`kheap_init`) so tests are
order-independent; re-init only resets pointers (cheap; leaked pages are
irrelevant given ~244 MiB free).

PMM:
- pages are page-aligned (`addr & 0xFFF == 0`) and consecutive allocs are
  `0x1000` apart;
- a freed page is returned by the next `pmm_alloc` (free-list reuse);
- `pmm_alloc_pages(3)` returns an aligned run; the next `pmm_alloc` is 3 pages on.

Heap:
- `kmalloc` returns non-NULL, 8-byte-aligned memory that survives write/read;
- a freed block is reused by an equal-size `kmalloc`;
- freeing two adjacent blocks lets an over-sized `kmalloc` succeed at the merged
  address (coalescing).

`int run_self_tests(void)` (declared in `tests.h`) wraps the array and returns
the failed-test count.

### CI exit code (`semihost.c`)

`void qemu_exit(int code)` uses the ARM **semihosting** `SYS_EXIT` operation:
load `x0 = 0x18` (SYS_EXIT) and `x1 = &{ ADP_Stopped_ApplicationExit (0x20026),
code }`, then `hlt #0xF000`. With QEMU's `-semihosting`, QEMU terminates with
`code` as its process exit status. If semihosting is disabled the function falls
through to a `wfi` halt loop (so it's safe to compile in always).

### Boot integration (`kmain.c`)

After `mmu_init`, call `run_self_tests()`:
- `#ifdef TEST_EXIT` (the `make test` build): `qemu_exit(failed ? 1 : 0)`.
- else (normal `make run`): if `failed`, print `SELF-TESTS FAILED -- halting.`
  and spin in `wfi`; otherwise continue to the demos.

### Makefile `test` target

```
test:
    $(MAKE) clean
    $(MAKE) EXTRA_CFLAGS=-DTEST_EXIT $(TARGET)
    $(QEMU) $(QEMU_FLAGS) -semihosting ; capture status
    $(MAKE) clean          # never leave TEST_EXIT objects around
    exit <status>
```
`CFLAGS` gains `$(EXTRA_CFLAGS)`. Cleaning before and after keeps the test build
from contaminating a normal `make run`. `-semihosting` is added only here.

## Part 2 — TDD enforcement

### Pre-commit hook (mechanical green-bar gate)

- `.githooks/pre-commit` (tracked in the repo) runs `make test`; on non-zero it
  prints a short message and exits non-zero, aborting the commit. Red builds
  cannot be committed.
- Activated with `git config core.hooksPath .githooks` (run once during setup).
- Reuses the same `make test` exit-code path — one source of truth.

### Workflow (test-first discipline)

- `docs/tdd-workflow.md` documents the loop for this kernel: add a failing
  `KASSERT` test → `make test` shows RED → implement → `make test` shows GREEN →
  refactor.
- All future feature plans order tasks test-first: the failing-test task precedes
  the implementation task, and we run it to observe RED before implementing.

## New files & changes

| File | Responsibility |
|------|----------------|
| `src/ktest.h` / `ktest.c` | KASSERT + the test runner |
| `src/tests.h` / `tests.c` | Test cases + `run_self_tests()` |
| `src/semihost.h` / `semihost.c` | `qemu_exit()` via ARM semihosting |
| `src/kmain.c` | (modified) run tests after `mmu_init`; halt-on-fail / exit in test mode |
| `Makefile` | (modified) `EXTRA_CFLAGS`, `test` target with `-semihosting` + exit code |
| `.githooks/pre-commit` | (new) run `make test`; block commit on failure |
| `docs/tdd-workflow.md` | (new) the red→green→refactor workflow |

All new code heavily commented (project preference).

## Success criteria

- `make run`: prints `[PASS]` for all tests + a summary, then proceeds to the
  demos and ticks.
- `make test`: prints results and the shell exit code is `0` when all pass;
  temporarily breaking a test yields a non-zero exit code (and, in a normal
  build, halts instead of continuing).
- With the hook active, committing while a test fails is rejected; committing
  with all green succeeds.

## Out of scope (for now)

Capturing `kprintf` output to test formatting; host-side unit tests; mocking
hardware; a tests-first heuristic in the hook (we chose the green-bar gate only).
Each can be added later.
