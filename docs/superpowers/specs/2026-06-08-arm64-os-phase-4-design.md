# ARM64 Learning OS — Phase 4 Design (Kernel Memory Allocation)

**Date:** 2026-06-08
**Status:** Approved

## Goal

Give MyOSv2 dynamic memory in two layers: a **physical page allocator (PMM)**
that owns free RAM and hands out 4 KiB pages, and a **heap (`kmalloc`/`kfree`)**
on top that serves arbitrary byte-sized requests by borrowing pages from the PMM.
Demonstrate both, including page reuse, block reuse, and coalescing.

Builds on Phase 0–3 (UART, exceptions, timer, MMU). Runs at EL1 with the MMU on.

## Architecture

```
 kmalloc(size) / kfree(ptr)         arbitrary sizes (bytes)
      |  kheap.c  (free-list heap; first-fit, split, coalesce)
      |  grows by requesting whole pages when low
      v
 pmm_alloc() / pmm_free()           fixed 4 KiB pages
      |  pmm.c  (Physical Memory Manager: bump pointer + free list)
      v
 free RAM: [__kernel_end .. RAM_END)
```

The two layers solve different problems and are independently testable: the PMM
manages physical RAM at page granularity (the unit the MMU uses); the heap carves
those pages into byte-sized allocations.

## Layer 1 — Physical Memory Manager (`pmm.c`)

- **PMM = Physical Memory Manager** (documented in the file header).
- **Pool:** all RAM from the end of the kernel image to the end of RAM.
  `linker.ld` gains a `__kernel_end` symbol (end of code+data+bss+stack);
  round it up to 4 KiB. `RAM_END` is fixed at `0x50000000` and QEMU is pinned to
  `-m 256M` so RAM is `0x40000000`–`0x50000000` (deterministic, matches the
  Phase 3 identity map of 1–2 GiB).
- **Algorithm — bump + free list:**
  - A bump pointer sweeps forward through never-used RAM (`pmm_alloc` advances it).
  - Freed pages go on an intrusive free list: the "next free page" pointer is
    stored *inside the free page itself*. `pmm_alloc` pops the free list first,
    only bumping when it is empty.
- **API:**
  - `void pmm_init(void)` — set the bump pointer to `align_up(__kernel_end, 4096)`.
  - `void *pmm_alloc(void)` — one 4 KiB page (page-aligned), or `NULL` if out of RAM.
  - `void pmm_free(void *page)` — push a page onto the free list.
  - `void *pmm_alloc_pages(size_t n)` — `n` contiguous pages via the bump pointer
    (used by the heap to grow); `NULL` if insufficient contiguous RAM.
- **Note/simplification:** `pmm_alloc_pages` always bumps (ignores the free list),
  so contiguous runs come from the never-used frontier. Documented in the file.

## Layer 2 — Heap (`kheap.c`)

- **Block layout:** a doubly-linked list of blocks in address order. Header:
  ```c
  struct block { size_t size; int free; struct block *next, *prev; };
  // payload follows immediately after the header
  ```
  `size` is the payload size (excluding the header). `kmalloc` returns the
  address just past the header; `kfree` subtracts the header size to recover it.
- **Operations:**
  - **First-fit allocate:** walk the list for a free block with `size >= request`.
    Requests are rounded up to an 8-byte boundary.
  - **Split:** if the chosen block is larger than needed by at least
    `sizeof(struct block) + 8`, split off the remainder as a new free block.
  - **Grow:** if no block fits, request enough pages from the PMM
    (`pmm_alloc_pages`), wrap them as a new free block, link it at the end, retry.
  - **Coalesce on free:** mark the block free; if `prev`/`next` are free *and
    physically adjacent* (header-of-neighbor == end-of-this), merge them.
- **API:**
  - `void kheap_init(void)` — start empty (first `kmalloc` grows it).
  - `void *kmalloc(size_t size)` — 8-byte-aligned pointer, or `NULL`.
  - `void kfree(void *ptr)` — free; ignores `NULL`.

## Demonstration (in `kmain`)

- **PMM:** allocate 3 pages, print addresses (4 KiB / `0x1000` apart); free the
  middle; allocate again → the freed page is returned (free list works).
- **Heap:** `kmalloc` a few sizes, print pointers, write/read through them;
  `kfree` one and re-`kmalloc` the same size → same address (reuse); then free two
  adjacent blocks and allocate a larger block that only fits if they coalesced.
- Timer ticks continue afterward (nothing destabilized).

## New files & changes

| File | Responsibility |
|------|----------------|
| `src/pmm.h` / `pmm.c` | Physical 4 KiB page allocator (bump + free list) |
| `src/kheap.h` / `kheap.c` | `kmalloc`/`kfree` free-list heap (first-fit, split, coalesce) |
| `linker.ld` | (modified) add `__kernel_end` symbol after the stack |
| `Makefile` | (modified) add `-m 256M` so RAM size is fixed and known |
| `src/kmain.c` | (modified) `pmm_init()`, `kheap_init()`, run the demos |

All new code is heavily commented (teaching-level), per project preference.

## Success criteria

- PMM returns distinct page-aligned addresses `0x1000` apart; a freed page is
  reused by the next `pmm_alloc`.
- Heap: written values read back correctly; a freed block is reused; a large
  request succeeds after two neighbors are freed and coalesced.
- `tick N` output continues after the demos.

## Out of scope (later phases)

Alignment beyond 8 bytes, `realloc`/`calloc`, slab caches, per-CPU caches,
returning whole freed pages from the heap back to the PMM, robust misuse
detection. Added later only if a phase needs them.
