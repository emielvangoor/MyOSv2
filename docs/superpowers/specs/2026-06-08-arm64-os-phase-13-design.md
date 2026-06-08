# MyOSv2 — Phase 13 Design (process lifecycle: exec + exit status + wait)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Finish the Unix process model. Today we have `fork` (Phase 9) but a process can't
**replace its image** (`exec`), can't return an **exit status**, and is never
**reaped** — so kernel stacks, address spaces, and ASIDs leak. This phase adds
`exec`, `exit(status)`, and `wait`, with full teardown (`as_destroy`) and ASID
recycling — closing the limitation noted at the end of Phase 11.

Builds on fork (9), the shell (10), ASIDs (11).

## New syscalls

```
SYS_EXEC  = 11   // x0 = path; replace current image. Returns only on failure (-1).
SYS_WAIT  = 12   // x0 = int *status; block for a child to exit. Returns child pid or -1.
```
`SYS_EXIT` (4) gains meaning for its argument: `x0` is the exit status.

## Thread lifecycle (`sched.c`)

`struct thread` gains:
```c
struct thread *parent;   // who created us (NULL for the boot/idle thread)
int            exit_status;
```
A new state **`THREAD_ZOMBIE`** (exited, awaiting reap). `parent` is set in
`thread_create`, `thread_create_image`, and `sched_fork` to the creating thread.

- **`void thread_exit(int status)`** — store `exit_status`, set state `ZOMBIE`,
  wake the parent if it's blocked in `wait`, then `schedule()` away (never
  returns). The thread stays in the run-ring as a zombie until reaped; the
  scheduler already skips non-`RUNNABLE` threads.
- **`int sched_wait(int *status)`** — from the calling (parent) thread:
  - scan the ring for a `ZOMBIE` whose `parent == current`; if found, write its
    status, **reap** it, return its pid;
  - else if the parent has any live children, block (sleep) until one exits, then
    retry;
  - else (no children) return -1.
- **Reaping** (`sched_wait`, IRQ-masked): unlink the zombie from the circular
  ring, `kfree` its kernel stack, `as_destroy(child->as)` if it has one, then
  `kfree` the thread struct.

`SYS_EXIT` calls `thread_exit(x0)`. (Orphan reparenting / a real `init` reaper is
out of scope; an orphaned zombie simply lingers — noted below.)

## Address-space teardown (`vm.c`)

**`void as_destroy(struct addrspace *as)`** frees everything a process owns:
- Walk the **user** subtree only — `l0[1]` (USER region); `l0[0]` is the *shared*
  kernel mapping and must NOT be freed. For each present L1→L2→L3 table, and each
  present L3 page, `page_decref` the physical page (so COW-shared pages survive in
  the other owner) and `pmm_free` the table pages.
- `pmm_free(as->l0)`; recycle the ASID (`asid_free(as->asid)`); `pmm_free(as)`.

**ASID recycling (`vm.c`)** — a small free list:
```c
void     asid_free(uint16_t a);     // push onto the free list
uint16_t asid_alloc(void);          // pop a freed ASID first, else bump next_asid
```
`vm_init()` resets both the counter and the free list.

## exec (`proc.c`, `syscall.c`)

**`int proc_exec(struct trapframe *tf, const char *path)`**:
1. `vfs_open(path)`; if missing → return -1 (caller's image untouched).
2. Read the whole file into a kernel buffer; `as_create_image(buf, len)` → new AS;
   free the buffer. On any failure → return -1.
3. Swap: remember `old = current->as`; `current->as = new`; `as_switch(new)`.
4. **Rewrite the trap frame** so the syscall return (`kernel_exit`/`eret`) lands
   in the fresh program: zero `x[0..30]`, `elr = user_entry_va()`
   (USER_CODE_VA), `sp_el0 = USER_STACK_TOP`, `spsr = 0` (EL0t, IRQs on — matching
   `enter_user`).
5. `as_destroy(old)`. Open file descriptors are **preserved** across exec (kept in
   the thread's `fds[]`).

`SYS_EXEC` calls `proc_exec(tf, path)`; on failure sets `x0 = -1` and returns
normally (the program continues). On success the rewritten trap frame means the
old code never resumes.

## User side (`ulib`, `sh.c`)

`ulib`: add `long sys_exec(const char *path)` and `long sys_wait(int *status)`.

Shell: an **unrecognized command** now does the Unix dance —
`fork()`; in the child `exec("/bin/<cmd>")` then `exit(127)` if exec returns
(not found); the parent `wait()`s and prints the child's exit status. With no
`/bin/<cmd>` programs yet (they arrive in Phase 14), this reports "not found
(127)" — but it exercises **fork → exec → exit → wait → reap** end to end, and
the same code runs real programs once Phase 14 ships coreutils. A `spawn` builtin
that forks a child which exits with a fixed status demonstrates the status path
visibly.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/sched.h`/`sched.c` | `THREAD_ZOMBIE`, `parent`/`exit_status`, `thread_exit(int)`, `sched_wait` + reap |
| `src/vm.h`/`vm.c` | `as_destroy`, `asid_free`, recycling in `asid_alloc`, `vm_init` reset |
| `src/proc.h`/`proc.c` | `proc_exec` |
| `src/syscall.h`/`syscall.c` | `SYS_EXEC`, `SYS_WAIT`, `SYS_EXIT` takes status |
| `user/syscalls.h`, `user/ulib.*` | `sys_exec`, `sys_wait` |
| `user/sh.c` | fork+exec+wait for external commands; `spawn` demo |
| `src/tests.c` | lifecycle tests (test-first) |
| `docs/notes/phase-13.md` | notes |

## Testing (test-first, deterministic, kernel-level)

EL0 exec/return is observed live; the kernel mechanics are unit-tested:

1. `test_asid_free_recycles` — `vm_init`; `a = asid_alloc()`; `asid_free(a)`; the
   next `asid_alloc()` returns `a`.
2. `test_as_destroy_frees_pages` — build an AS; record `pa = as_translate(as,
   USER_DATA_VA)` (refcount 1); `as_destroy(as)`; `page_refcount(pa) == 0`.
3. `test_as_destroy_recycles_asid` — `as_destroy(as)` makes `as->asid` available:
   the next `asid_alloc()` returns it.
4. `test_wait_reaps_child` — `sched_init`; `thread_create` a child that calls
   `thread_exit(7)`; from the parent `sched_wait(&st)` returns the child's id with
   `st == 7`, and the child is gone from the ring (a second `sched_wait` returns
   -1 — no children left).
5. `test_wait_no_children` — `sched_wait(&st)` returns -1 when current has none.
6. `test_exec_missing_returns_neg1` — `proc_exec(&tf, "/nope")` returns -1 and
   leaves `current`'s address space unchanged.

## Success criteria

- 6 lifecycle tests pass (test-first); all prior tests stay green; the gate holds.
- Live: the shell runs `spawn` (a child exits with a status the shell prints) and
  reports unknown commands via the fork→exec→wait path; repeated spawns don't leak
  (ASIDs/pages recycled).

## Out of scope

`execve` argv/envp (path only for now); orphan reparenting to `init`;
`waitpid(specific pid)` / `WNOHANG`; close-on-exec flags; threads sharing an
address space. These can come later.
