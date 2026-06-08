# Phase 16 notes — mmap + shared memory

## What changed

Two general memory facilities: anonymous **`mmap`** (a fresh, demand-zeroed
region at a kernel-chosen address) and **shared memory** (physical pages mapped
into several processes at once). `/bin/shmtest` shows two processes communicating
through a shared page:

```
$ shmtest
shmtest: ok        # child writes a sentinel; parent reads it back via the same page
```

## Layout + mmap

A new per-AS region, `USER_MMAP_BASE = 0x8000400000`, with a bump pointer
`mmap_next`. `as_mmap(len)` allocates fresh zeroed pages, maps them RW + no-exec,
and returns the base VA; `as_munmap` drops the mappings and references. Like the
heap, these pages live under `l0[1]`, so `as_destroy` frees any still mapped.

## Shared memory

A small table of objects (`shm.c`), each owning a set of physical pages.
`shm_create(len)` allocates the pages and takes **the table's own reference** on
each (so they persist even when nobody currently maps them). `shm_map(as, h)`
installs those *same* physical pages into `as` (via `as_map_phys`), bumping each
page's refcount. Two processes that map the same handle share the pages — a write
by one is seen by the other. A mapper exiting (`as_destroy`) only drops its
reference; the table's reference keeps the object alive.

The unit test pins the essence: map one handle into two address spaces,
`as_translate` resolves both to the **same** physical page, and a byte written
through one is read back through the other.

## The fork bug this surfaced (worth remembering)

`shmtest` initially failed because **`as_clone` (fork) never initialized the
child's `heap_base`/`heap_end`/`mmap_next`** — those fields were added in
Phases 15–16, after `as_clone` was written. The child inherited garbage, so its
`shm_map`/`malloc` computed bogus addresses. Fix: `as_clone` now copies the
parent's heap break and mmap position into the child. (This also silently fixed
`malloc` in any forked child.)

`shmtest` maps the object **after** the fork in both parent and child, so the
mappings are fresh RW — mapping *before* fork would let copy-on-write privatize
them. Marking shared VMAs to skip COW is a real-OS feature left out here.

## Testing

6 kernel tests, test-first: mmap maps a zeroed page; two mmaps return distinct
pages; munmap unmaps; shm_create returns a handle; a handle maps to the same
physical page in two address spaces (write-visible both ways); the object
survives a mapper exiting. The cross-process flow is verified live by
`/bin/shmtest`.

## Limits

Anonymous mmap only (no file-backed); no address hint / prot argument; shm
objects aren't destroyed (handles + pages leak for the run); shared mappings
don't survive fork un-privatized.
