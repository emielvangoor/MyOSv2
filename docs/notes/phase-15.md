# Phase 15 notes — user-space dynamic memory (sbrk + malloc)

## What changed

User programs can allocate memory at runtime. The kernel grows a per-process
heap on demand (`sbrk`); a small user-space `malloc`/`free` sits on top.
`/bin/mtest` exercises it:

```
$ mtest
mtest: ok
[exit 0]
```

## The heap + sbrk

Each address space tracks `heap_base`/`heap_end` (the program break), both
starting at **`USER_HEAP_BASE = 0x8000200000`** — above the stack, clear of code
and data. `as_sbrk(as, incr)` returns the *old* break and grows `heap_end`;
any page newly covered by `[heap_base, heap_end)` is freshly allocated, **zeroed**
(demand-zero), and mapped RW + no-exec. These heap pages live under `l0[1]`, so
`as_destroy` already frees them on exit. `SYS_SBRK` exposes it to EL0.

## User malloc

A minimal first-fit allocator in `ulib`: each block is `[header | payload]` with
`{ size, next }`. `malloc` reuses a fitting freed block or calls `sbrk` to grow
the heap and carve a new one; `free` pushes the block onto a free list. No
coalescing or best-fit — simple and good enough.

## The W^X bug this surfaced (worth remembering)

Adding malloc's free-list global gave every program a writable data section. The
linker had been merging it with `.text` into a single **RWE** segment. Two things
broke:
1. `as_clone` (fork) classifies a page as "writable" by its `AP_RW_ALL` bits — so
   it saw the *code* page as writable and demoted it to **copy-on-write +
   UXN** (no-execute). The next instruction fetch after any syscall then faulted.
2. It violated W^X (writable *and* executable).

**Fix:** force separate `PT_LOAD` segments — a read-only/executable text segment
and a read-write (non-exec) data segment — via explicit `PHDRS` in `user.ld` and
`-z max-page-size=0x1000` (so segments align to our 4 KiB page, not the AArch64
default 64 KiB). Also made `as_map_segment`'s `exec` flag independent of
`writable`. Now code is RO+exec and fork shares it as-is (no COW demotion), while
data COWs correctly.

## Testing

5 kernel sbrk tests, test-first: initial break is `USER_HEAP_BASE`; a grow
returns the old break and maps a demand-zeroed page; the break advances; a large
grow spans multiple pages. The user-space allocator is verified live by
`/bin/mtest` (non-overlapping blocks, free+reuse, multi-page allocation).

## Limits

No `mmap`-backed allocation (Phase 16); heap shrink lowers the break but leaves
pages mapped; no coalescing/best-fit; single-threaded per process.
