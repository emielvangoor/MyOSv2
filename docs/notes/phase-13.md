# Phase 13 notes — process lifecycle (exec + exit + wait)

## What changed

The Unix process model is now complete. A process can replace its image
(`exec`), exit with a **status**, and be **reaped** by its parent (`wait`) — and
all of its resources (kernel stack, user pages, page tables, ASID) are actually
freed. Live demo in the shell:

```
$ spawn
  [child] running, exiting with status 3
  [parent] reaped pid 2
  [parent] child status 3
$ foobar
exec: not found          # fork -> exec(/bin/foobar) fails -> child exit(127) -> wait
```

## exec (the trap-frame rewrite)

`proc_exec(tf, path)` runs *inside the syscall* on the caller's kernel stack. It
loads the file into a new address space (`as_create_image`), switches `TTBR0` to
it, then **rewrites the saved trap frame** so the syscall's `eret` lands in the
fresh program: registers cleared, `elr = USER_CODE_VA`, `sp_el0 =
USER_STACK_TOP`, `spsr = 0` (EL0t, IRQs on — matching `enter_user`). The old
address space is then destroyed. The kernel stack we're standing on lives in
kernel memory, not the user AS, so tearing down the old AS underneath us is safe.
Open fds are preserved. On a missing/unloadable file it returns -1 with nothing
touched.

## Zombies + reaping

`thread_exit(status)` records the status, marks the thread `THREAD_ZOMBIE` (the
scheduler skips it), and wakes the parent if it's blocked in `wait`. The thread
stays in the run-ring as a tombstone until reaped.

`sched_wait(status)` scans the ring for a zombie whose `parent == current`. If
found it writes the status, **unlinks** the zombie from the circular ring (using
the predecessor found during the scan), and frees its kernel stack + address
space + thread struct. If the caller has live-but-not-yet-exited children it
blocks (sleeps) until one exits; with no children it returns -1.

## as_destroy + ASID recycling

`as_destroy` walks **only** the user subtree (`l0[1]`; `l0[0]` is the shared
kernel map and must never be freed). For every present L3 page it `page_decref`s
the physical page — so a COW-shared page survives in its other owner — then frees
the L3/L2/L1 table pages, the L0 page, and the struct. The ASID goes onto a small
**free list** that `asid_alloc` pops from before bumping the counter, so a
fork/exec/exit-heavy workload recycles IDs instead of exhausting them (closing the
limitation noted at the end of Phase 11).

## Testing

6 kernel-level tests, test-first: ASID free-list recycle; `as_destroy` returns a
page to the PMM (refcount → 0) and recycles its ASID; `sched_wait` reaps a child
and returns its status, then reports "no children"; `wait` with no children → -1;
`exec` of a missing path → -1. The EL0 side (exec actually entering a new image)
is verified live in the shell.

## Limits

`execve` argv/envp (path only); no orphan reparenting to an `init` reaper (an
orphaned zombie lingers); no `waitpid(pid)`/`WNOHANG`; no close-on-exec. These can
come later.
