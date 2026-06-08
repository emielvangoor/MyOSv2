# MyOSv2 — Phase 16 Design (mmap + shared memory)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Two general-purpose memory facilities:
1. **anonymous `mmap`** — map a fresh, demand-zeroed region at a kernel-chosen
   address (generalizing the heap; a place for large/standalone allocations).
2. **shared memory** — a named object whose physical pages can be mapped into
   **several processes at once**, so they share writes. This is the building
   block for IPC and any future user-space service.

Builds on address spaces (6b), page refcounts (9), and the heap (15).

## Address-space layout

A new region for mmap/shm mappings, above the heap:
```
USER_HEAP_BASE 0x8000200000  heap (sbrk), grows up
USER_MMAP_BASE 0x8000400000  mmap + shm mappings, bump up   <-- new
```
`struct addrspace` gains `uint64_t mmap_next;` (initialized to `USER_MMAP_BASE`),
a simple bump allocator for mapping addresses.

## Anonymous mmap (`vm.c`)

```c
uint64_t as_mmap(struct addrspace *as, uint64_t len);          // -> base VA (0 on fail)
int      as_munmap(struct addrspace *as, uint64_t va, uint64_t len);
```
`as_mmap` rounds `len` up to pages, allocates that many fresh **zeroed** pages,
maps them RW + no-exec at `mmap_next`, advances `mmap_next`, and returns the base
VA. `as_munmap` unmaps the range and `page_decref`s each page. These pages live
under `l0[1]`, so `as_destroy` frees any still-mapped on exit.

## Shared memory (`shm.c`)

A small fixed table of shared objects, each owning a set of physical pages:
```c
int      shm_create(uint64_t len);                 // -> handle id (>=0), or -1
uint64_t shm_map(struct addrspace *as, int handle);// map into `as` -> base VA (0 on fail)
```
- `shm_create` allocates `ceil(len/PAGE)` physical pages, **`page_incref`s each
  once** (the table's own reference, so the pages persist even when no process
  currently maps them), and returns a handle.
- `shm_map` maps the object's pages into `as` at `mmap_next` (RW, no-exec),
  `page_incref`ing each, and returns the base VA. Two processes that map the same
  handle get the **same physical pages** — a write by one is seen by the other.

`as_destroy` `page_decref`s a process's mappings; the table's own reference keeps
the pages alive for the object's lifetime (objects aren't destroyed yet — noted
below).

Limits (kept small + simple): up to 16 objects, up to 16 pages (64 KiB) each.

## Syscalls

```
SYS_MMAP       = 14   // x0 = len            -> base VA (or -1)
SYS_MUNMAP     = 15   // x0 = va, x1 = len   -> 0 / -1
SYS_SHM_CREATE = 16   // x0 = len            -> handle (or -1)
SYS_SHM_MAP    = 17   // x0 = handle         -> base VA (or -1)
```

## User side (`ulib`) + demo

Wrappers `mmap`/`munmap`/`shm_create`/`shm_map`. A `/bin/shmtest` program
demonstrates cross-process sharing **the fork-safe way** (map *after* fork, so the
mappings are fresh RW, not copy-on-write):
1. parent `h = shm_create(4096)`;
2. `fork()`; the child inherits `h`;
3. child `shm_map(h)`, writes a sentinel value, exits;
4. parent `wait`s, then `shm_map(h)` and reads the sentinel — it sees the child's
   write. Prints `shmtest: ok` (exit 0) or a failure.

(Mapping after fork avoids the COW-demotion that would privatize a mapping
inherited across fork — a real OS marks shared VMAs to skip COW; out of scope.)

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/vm.h`/`vm.c` | `mmap_next`, `USER_MMAP_BASE`, `as_mmap`/`as_munmap` |
| `src/shm.h`/`shm.c` | shared-object table, `shm_create`/`shm_map` |
| `src/syscall.h`/`syscall.c` | 4 new syscalls |
| `user/syscalls.h`, `user/ulib.*` | wrappers |
| `user/shmtest.c` | shared-memory demo (test program) |
| `Makefile`, `src/initrd.c` | build + register `/bin/shmtest` |
| `src/tests.c` | mmap + shm tests (test-first) |
| `docs/notes/phase-16.md` | notes |

## Testing (test-first, kernel-level)

1. `test_mmap_maps_zeroed` — `as_mmap(as, 100)` returns a VA ≥ `USER_MMAP_BASE`,
   `as_translate` is non-zero there, and the first byte reads 0.
2. `test_mmap_two_distinct` — two `as_mmap` calls return different VAs mapping
   **different** physical pages.
3. `test_munmap_unmaps` — after `as_munmap(va, len)`, `as_translate(as, va) == 0`.
4. `test_shm_create_handle` — `shm_create(4096)` returns a handle ≥ 0.
5. `test_shm_shared_pages` — map one handle into two address spaces;
   `as_translate(as1, va1) == as_translate(as2, va2)` (same physical page); a byte
   written through `as1`'s page is read back through `as2`'s page.
6. `test_shm_survives_unmap` — `shm_create`, map into `as` (refcount 2: table +
   map), `as_destroy(as)`; the page is still alive (`page_refcount > 0`) thanks to
   the table's reference; a fresh `shm_map` still works.

## Success criteria

- 6 tests pass (test-first); prior tests stay green; gate holds.
- Live: `shmtest` prints `shmtest: ok` (a child and parent communicate through a
  shared page); `mmap` works for a large anonymous allocation.

## Out of scope

File-backed `mmap` (anonymous only); `mmap` with a fixed/​hinted address or
permissions argument; shared mappings surviving `fork` un-privatized; destroying
shm objects / reclaiming handles; `MAP_SHARED` vs `MAP_PRIVATE` semantics.
