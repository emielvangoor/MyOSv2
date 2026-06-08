# MyOSv2 — Phase 9 Design (fork + copy-on-write)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

`fork()`: duplicate the current process. The child gets a copy of the parent's
address space and file descriptors, and returns 0; the parent gets the child's
pid. The address-space copy is **copy-on-write (COW)**: pages are shared
read-only and copied lazily on the first write (a data-abort fault).

Builds on Phase 6b (address spaces) and Phase 8 (processes, fd tables).

## Page reference counts (`vm.c`)

A page can be shared by several address spaces (COW), so we count references:
```c
static uint16_t page_ref[ (RAM_END-RAM_BASE)/PAGE ];   // index = (pa-RAM_BASE)>>12
void page_incref(uint64_t pa);   // ++
void page_decref(uint64_t pa);   // --; pmm_free when it hits 0
```
`as_create_image`'s freshly-allocated user pages start with refcount 1.

## COW descriptors

A spare software bit in the page descriptor marks a COW page:
```c
#define PTE_COW (1UL << 55)     // software-use bit: "copy on write"
```
On `as_clone`, every **writable user page** (stack, data) in *both* parent and
child is remapped **read-only + COW**, and its refcount is bumped. Read-only code
pages are shared as-is (never written). The TLB is flushed.

## as_clone (`vm.c`)

`struct addrspace *as_clone(struct addrspace *parent)`:
- new L0; `l0[0] = mmu_kernel_l0_entry()` (shared kernel map).
- Walk the parent's user mappings (USER_CODE / USER_STACK range / USER_DATA). For
  each present page: map the **same physical page** in the child; if it was
  writable, mark both parent and child entries read-only + COW and `page_incref`.
- Returns the child address space.

## COW fault (`vm.c`, `exceptions.c`)

On a **write to a read-only COW page**, the data-abort handler copies it:
`int cow_fault(struct addrspace *as, uint64_t va)`:
- find the page entry for `va`; if not present or not COW → return 0 (not a COW
  fault, real fault).
- allocate a fresh page, copy the old page's bytes, map the new page **read/write**
  at `va` (clear COW), `page_decref` the old page, flush TLB; return 1.

`el0_sync_handler`: if the exception class is a data abort from EL0 (`EC 0x24`) and
the access was a write (`ESR.WnR`), call `cow_fault(current->as, FAR_EL1)`; if it
handled it, just return (retry the store). Otherwise it's a real fault → kill.

## fork (`syscall.c`, `sched.c`)

`SYS_FORK (9)` — runs in `do_syscall` with the parent's trap frame `tf`:
- create a child thread: `as = as_clone(parent->as)`; copy the fd table
  (sharing the same `struct file*`); same priority.
- the child must resume at EL0 exactly where the parent's `svc` returns, but with
  `x0 = 0`. We copy the parent's trap frame onto the child's kernel stack with
  `x[0] = 0`, and set the child's saved context so that when first scheduled it
  runs `fork_ret` (assembly) = the `kernel_exit` sequence, restoring that trap
  frame and `eret`-ing to EL0.
- the parent's `SYS_FORK` returns the child's pid.

`thread_create_forked(struct addrspace *as, struct trapframe *parent_tf, int prio)`
in `sched.c` builds that child thread.

## Demo (`kmain` / a user program)

A user program calls `fork()`; parent and child each write a different value to a
shared data page and read it back — each sees **its own** value (COW gave them
private copies). Reported via `SYS_REPORT`. Observed in the demo.

## Testing (test-first, deterministic — COW at the page level)

1. `test_cow_clone_shares_pages` — after `as_clone`, parent and child map
   `USER_DATA_VA` to the **same** physical page.
2. `test_cow_clone_refcount` — that shared page's refcount is 2.
3. `test_cow_fault_copies` — `cow_fault(child, USER_DATA_VA)` makes the child map a
   **different** physical page (private), with the old contents preserved.
4. `test_cow_fault_refcount` — after the copy, the original page's refcount is 1.
5. `test_cow_fault_non_cow` — `cow_fault` on a non-COW VA returns 0.

(Actual fork at EL0 is observed in the demo.)

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/vm.h`/`vm.c` | `page_incref/decref`, `as_clone`, `cow_fault`, `PTE_COW` |
| `src/exceptions.c` | data-abort COW handling in `el0_sync_handler` |
| `src/sched.h`/`sched.c` | `thread_create_forked` |
| `src/syscall.h`/`syscall.c` | `SYS_FORK` |
| `src/switch.S` | `fork_ret` (kernel_exit entry for a forked child) |
| `user/prog.c` (+ ulib) | a fork demo program; `sys_fork` |
| `src/tests.c` | 5 COW tests (test-first) |
| `docs/notes/phase-9.md` | notes |

## Success criteria

- 5 COW tests pass (test-first); the demo shows a forked parent+child with
  independent memory (COW); prior tests stay green; gate intact.

## Out of scope

`wait`/`waitpid`/zombies/reaping (the demo child just exits); `exec` replacing the
image after fork (Phase 10 shell uses spawn); shared file-offset semantics; signals.
