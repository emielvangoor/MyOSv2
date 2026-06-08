# Phase 4 notes — Kernel Memory Allocation (PMM + heap)

## What changed

MyOSv2 now has dynamic memory in two layers: a Physical Memory Manager (PMM)
that hands out 4 KiB pages of RAM, and a `kmalloc`/`kfree` heap on top that
serves arbitrary byte-sized requests.

Observed run:
```
PMM: three pages -> 0x4008d000 0x4008e000 0x4008f000 (each 0x1000 apart)
PMM: freed middle page, next alloc -> 0x4008e000 (reused!)
heap: a=0x40090020 b=0x40090060 c=0x400900c0
heap: freed b, kmalloc(64) -> 0x40090060 (reused!)
heap: freed a+b (merged); kmalloc(80) -> 0x40090020 (coalesced!)
```

## Why two layers

They solve different problems:
- The **PMM** owns physical RAM and deals in whole 4 KiB **pages** -- the same
  unit the MMU maps. Coarse but simple.
- The **heap** carves those pages into **byte-sized** allocations for kernel
  data structures (lists, buffers, ...).

Real kernels layer it the same way (Linux: buddy page allocator -> slab/kmalloc).

## Where free RAM comes from

The linker script exports `__kernel_end` (first byte past code+data+bss+stack;
page-aligned). Free RAM is `[__kernel_end .. RAM_END)`. `RAM_END` is fixed at
`0x50000000` and QEMU is launched with `-m 256M`, so RAM is
`0x40000000`-`0x50000000`. The two MUST agree -- if you change `-m`, change
`RAM_END`. (Observed `__kernel_end` = `0x4008c000`; the first page handed out is
`0x4008d000`.)

## PMM internals (bump + free list)

- A **bump pointer** sweeps forward through never-used RAM; `pmm_alloc` returns
  the page at the frontier and advances it.
- Freed pages go on an **intrusive free list**: since the page's contents are
  unused, the "next free page" pointer is stored in the page's own first 8
  bytes. `pmm_alloc` pops this list before bumping, so freed pages get reused
  (that's why the freed middle page came back).
- `pmm_alloc_pages(n)` returns a contiguous run -- but always from the bump
  frontier (it ignores the free list, since freed pages may not be adjacent).

## Heap internals (first-fit, split, coalesce)

The heap is a doubly-linked list of blocks in address order. Each block:
```c
struct block { size_t size; int free; struct block *next, *prev; };  // payload follows
```
`sizeof(struct block)` is 32 bytes here (8 + 4 + pad 4 + 8 + 8), which is why the
demo addresses are spaced payload+32 apart.

- **First-fit:** walk the list for a free block with `size >= request` (requests
  rounded up to 8 bytes).
- **Split:** if the chosen block has room to spare (>= request + header + 8),
  carve the tail into a new free block so it isn't wasted.
- **Grow:** if nothing fits, ask the PMM for enough pages, wrap them as one big
  free block, and append it.
- **Coalesce on free:** mark free, then merge with the `prev`/`next` neighbor if
  it is also free AND physically adjacent (`adjacent()` checks
  `header + sizeof + size == next_header`). Merging is what let `kmalloc(80)`
  reuse the combined `a`+`b` space.

## Why the demo proves what it proves

- PMM pages `0x1000` apart -> page-granular allocation.
- Freed middle page returned -> free list reuse.
- `kmalloc(64)` after `kfree(b)` returns b's address -> block reuse.
- `kmalloc(80)` after freeing adjacent `a`+`b` returns a's address -> coalescing
  (without the merge, 80 fit in neither the 32 nor the 64 block, so it would
  have come from elsewhere with a different address).

## Known simplifications (revisit later if needed)

- `pmm_alloc_pages` ignores the free list (contiguous runs only from the frontier).
- The heap never returns whole freed pages back to the PMM (freed space stays in
  the heap's own free list).
- 8-byte alignment only; no `realloc`/`calloc`; minimal misuse checking.
