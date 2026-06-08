# MyOSv2 — Phase 15 Design (user-space dynamic memory: sbrk + malloc)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Let user programs allocate memory at runtime. The kernel grows a per-process
**heap** on demand (`sbrk`); a small user-space **`malloc`/`free`** sits on top.
Until now a program had only its fixed code/data/stack pages.

Builds on address spaces (6b) and ELF loading (14).

## Heap region

Each address space gets a heap that grows **up** from a fixed base, clear of the
other regions:
```
USER_CODE_VA   0x8000000000   code/rodata/data (ELF segments)
USER_STACK     ...0x8000100000 (16 pages, grows down)
USER_HEAP_BASE 0x8000200000   heap, grows up   <-- new
```
`struct addrspace` gains `uint64_t heap_base, heap_end;`, both initialized to
`USER_HEAP_BASE` when the address space is built (`as_create_elf` /
`as_create_image`). `heap_end` is the exact break; whole pages are mapped to
cover it.

## sbrk (`vm.c`)

```c
uint64_t as_sbrk(struct addrspace *as, long incr);
```
- Returns the **old** break (`heap_end`).
- Grows `heap_end` by `incr`. Any page newly covered by `[heap_base, heap_end)`
  is freshly allocated, **zeroed** (demand-zero), and mapped RW + no-exec + NG
  (user data). Pages already mapped are left alone.
- `incr == 0` just queries the current break.
- `incr < 0` lowers the break (the freed pages stay mapped for simplicity — a
  shrink that re-grows reuses them; full unmap is out of scope).
- On out-of-memory returns `(uint64_t)-1` with the break unchanged.

These heap pages live under `l0[1]`, so `as_destroy` already frees them on exit.

## Syscall

```
SYS_SBRK = 13   // x0 = signed increment -> previous break (or -1)
```
`do_syscall` calls `as_sbrk(sched_current_as(), (long)x0)`.

## User-space malloc (`ulib`)

`sys_sbrk(long)` wrapper, plus a minimal first-fit allocator:
- A singly-linked free list of blocks, each with a small header `{ size, next }`.
- `malloc(n)`: round up, search the free list for a fit; if none, `sbrk` more and
  carve a block. `free(p)`: push the block back onto the free list (adjacent-merge
  is optional and omitted for simplicity).
- `calloc`-style zeroing isn't needed (sbrk pages start zeroed), but fresh blocks
  from the free list may hold old data — `malloc` does not zero (callers that need
  zero use the fact that sbrk memory is zero on first use, like the kernel heap).

## Verifying malloc

A self-testing program `/bin/mtest`: allocate several blocks, write distinct
patterns, read them back (no overlap/corruption), free and re-allocate (reuse),
and a large allocation that forces `sbrk` to map multiple pages. It prints
`mtest: ok` and returns 0, or prints the first failure and returns 1. The shell
runs it via the Phase-13 fork→exec→wait path, so its result + exit status are
visible.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/vm.h`/`vm.c` | `heap_base`/`heap_end`, `USER_HEAP_BASE`, `as_sbrk`; init in AS builders |
| `src/syscall.h`/`syscall.c` | `SYS_SBRK` |
| `user/syscalls.h`, `user/ulib.*` | `sys_sbrk`, `malloc`/`free` |
| `user/mtest.c` | malloc self-test program |
| `Makefile` | add `mtest` to `PROGS` |
| `src/initrd.c` | register `/bin/mtest` |
| `src/tests.c` | sbrk tests (test-first) |
| `docs/notes/phase-15.md` | notes |

## Testing (test-first, kernel-level)

The kernel mechanism is unit-tested; malloc is verified by `/bin/mtest` live.

1. `test_sbrk_initial_break` — a fresh `as_create_elf` reports
   `as_sbrk(as, 0) == USER_HEAP_BASE`.
2. `test_sbrk_grows_and_maps` — `as_sbrk(as, 100)` returns `USER_HEAP_BASE`, and
   `as_translate(as, USER_HEAP_BASE)` is now non-zero (a page was mapped).
3. `test_sbrk_zeroed` — the first byte of the new heap page reads as 0.
4. `test_sbrk_multi_page` — `as_sbrk(as, 8192+1)` maps **three** pages:
   `as_translate` is non-zero at `USER_HEAP_BASE`, `+4096`, and `+8192`.
5. `test_sbrk_query_after_grow` — after growing by 100 then by 50, `as_sbrk(as,0)`
   equals `USER_HEAP_BASE + 150`.

## Success criteria

- 5 sbrk tests pass (test-first); prior tests stay green; gate holds.
- Live: `mtest` prints `mtest: ok` with exit status 0; large allocations work
  (multi-page sbrk); freed blocks are reused.

## Out of scope

`mmap`-backed allocation (Phase 16); heap shrink with real page unmapping;
coalescing/best-fit malloc; thread-safety (single-threaded per process).
