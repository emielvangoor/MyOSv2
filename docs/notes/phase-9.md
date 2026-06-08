# Phase 9 notes — fork + copy-on-write

## What changed

`fork()` duplicates a process. The child gets a copy-on-write clone of the
parent's address space + a copy of the fd table, returns 0; the parent gets the
child pid. Demo:

```
init: forking (testing copy-on-write)
parent: x=1     (parent wrote 1 to its private copy)
child:  x=2     (child wrote 2 to its private copy)
```

## Copy-on-write

- **Page reference counts** (`page_ref[]`, indexed by physical page) let a page be
  shared by several address spaces and freed only at the last reference.
- **`as_clone`** maps the child the SAME physical pages as the parent, and remaps
  every *writable* page (stack, data) **read-only + a software COW bit** in BOTH
  parent and child (code is read-only and just shared). Each shared page's
  refcount is bumped.
- A **write to a COW page** faults (data abort, EC 0x24, WnR=1). `el0_sync_handler`
  calls **`cow_fault`**, which allocates a fresh page, copies the contents, maps it
  read/write (clearing COW), drops the old page's refcount, and `eret`s to retry
  the store. Parent and child diverge lazily, only touching pages they write.

## fork's child resume (the tricky bit)

`SYS_FORK` runs on the parent's behalf with the parent's trap frame. The child
must resume at EL0 *right after the parent's `svc`*, but with `x0 = 0`. We copy the
parent's trap frame onto the child's kernel stack (x0 := 0) and set the child's
saved context so the first `cpu_switch` into it lands in `fork_ret` (= the
`kernel_exit` sequence), which restores that trap frame and `eret`s to EL0.

## A bug worth remembering

`as_translate`/COW extracted the physical address with `& ~0xFFF`, which KEEPS the
high attribute bits (UXN/PXN/COW at bits 53-55) -> the "pa" came out as garbage
(`0xe000...`). The physical address is bits **[47:12]**; the fix is a proper
`PA_MASK = 0x0000FFFFFFFFF000`. (This was a latent bug since Phase 6b that only
surfaced once descriptors carried high bits.)

## Testing

5 page-level COW tests (test-first): clone shares pages; refcount becomes 2;
`cow_fault` makes a private copy with contents preserved; the copy drops the
original's refcount; a non-COW fault returns 0. The EL0 fork is observed in the
demo.

## Limits

No `wait`/zombie reaping; no `exec` image-replace after fork (the shell uses
spawn); fd table is shared by pointer (no per-fd refcount); fixed user-VA set
walked on clone.
