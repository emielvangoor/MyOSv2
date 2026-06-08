# Testing notes — how MyOSv2 tests itself

## On-target vs host testing

Most kernel behavior depends on real hardware: privilege levels, the MMU, the
GIC, the timer, MMIO. None of that exists in a normal host process, so MyOSv2
runs its tests **on-target** -- inside the kernel, under QEMU, where the hardware
behaves like the real thing. (A future option is to also unit-test the *portable*
logic -- e.g. the heap algorithm with a fake page allocator -- on the host for a
faster loop. Not done yet.)

## The KTEST framework

- A test is a `void fn(void)`; assertions use `KASSERT(cond)`.
- `KASSERT` stringifies its condition (`#cond`) so a failure prints exactly what
  was checked, with `file:line`.
- `ktest_run()` runs each test with a per-test `current_failed` flag (reset before
  each test), prints `[PASS]`/`[FAIL]` and a summary, and returns the failed count.

## ARM semihosting and the exit code

`make test` needs the *shell* to learn pass/fail. The kernel can't call `exit()`
-- there's no OS under it -- so it uses **ARM semihosting**: a debug channel where
the guest asks QEMU to act on its behalf. `qemu_exit(code)` loads the SYS_EXIT
operation and a `{reason, code}` block, then runs `HLT #0xF000`. QEMU (started
with `-semihosting`) terminates with `code` as its process exit status. So:

```
all tests pass -> qemu_exit(0) -> QEMU exits 0 -> `make test` returns 0 (green)
any test fails -> qemu_exit(1) -> QEMU exits 1 -> `make test` returns non-zero (red)
```

In a normal (non-test) build the same failure instead HALTS the kernel, so you
can't reach the demo with a broken foundation.

## Why `make test` double-cleans

The test kernel is built with `-DTEST_EXIT` (so it exits QEMU instead of
continuing). If those object files were left behind, a later `make run` would
reuse them and exit immediately too. So `make test` runs `clean` before AND after,
keeping the test build isolated from the normal build.

## The commit gate

`.githooks/pre-commit` runs `make test` and aborts the commit on any non-zero
result. Enabled with `git config core.hooksPath .githooks` (we keep the hook in a
tracked directory so it's versioned and shared, unlike the untracked
`.git/hooks/`).

## Gotcha learned while building this

`git checkout <path>` restores a file from the **index**, not HEAD. If you've
staged a broken version, that won't undo it -- use
`git restore --source=HEAD --staged --worktree <path>` (or `git checkout HEAD --
<path>`) to truly revert.

## Known limitation

`kprintf` output formatting isn't tested yet (we'd need to capture output into a
buffer -- e.g. a `ksnprintf`). The format code is exercised indirectly by every
test's output, but not asserted on.
