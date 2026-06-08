# Phase 17 notes — pipes

## What changed

A **pipe**: a unidirectional in-kernel byte channel with a read end and a write
end. `read` blocks until data or all writers close (EOF); `write` blocks until
space or all readers close (broken pipe). With `dup2`, the shell wires one
command's stdout to the next's stdin:

```
$ hello | wc
22
```

(`hello` writes 22 bytes into the pipe; `wc` reads stdin to EOF and prints the
count.)

## The pieces

- **`pipe.c`** — a `PIPE_SIZE` ring buffer with `readers`/`writers` counts.
  Blocking is cooperative: an empty reader (with writers alive) `yield()`s and
  retries; a full writer (with readers alive) does the same. The pipe is freed
  when both end-counts reach 0.
- **`struct file`** gained `pipe`, `writable`, and a **reference count**. fds are
  shared across `fork` and `dup2`, so a file must outlive any single close:
  `vfs_close` decrements and frees at 0 (closing a pipe end then); `file_dup`
  increments; `sched_fork` `file_dup`s every inherited fd.
- **fd-first routing** — `SYS_WRITE`/`SYS_READ` now prefer an open `fds[fd]` and
  fall back to the UART console only for bare fd 0/1/2. So a pipe `dup2`'d onto
  stdin/stdout is used for I/O. `SYS_PIPE` allocates its ends from **fd 3 up** so
  it never clobbers the console fds. `thread_exit` closes the process's fds, so a
  writer exiting drops the pipe's writer count and the reader sees EOF.

## Three deep bugs this surfaced (worth remembering)

1. **`cow_fault` never `page_incref`'d the copied page** — the root cause of
   intermittent corruption. A copied page sat at refcount 0; the kernel's COW
   faults (see below) created such pages, a later `fork` shared them off-by-one,
   and destroying the child freed a page the parent still used. Fix: take a
   reference on the copy (and skip the copy entirely when sole owner).
2. **Kernel writes to user COW pages** — after a fork, a process's stack is COW;
   when the kernel writes into it (e.g. `sys_read` storing a keyboard byte), the
   store faults at **EL1** (EC 0x25). The EL1 sync handler now runs `cow_fault`
   and retries, instead of skipping the instruction.
3. **`as_clone` lost the COW bit on a second fork** and only walked a hardcoded
   VA list. Rewritten to walk the entire user page-table subtree and preserve
   each page's exact attributes — so an already-COW page stays COW, and heap/mmap
   regions are cloned too. Also: `as_destroy` now flushes the dying ASID's TLB
   entries before recycling it.

## Testing

5 kernel tests, test-first: pipe write-then-read, ring-buffer wrap, EOF when no
writers, broken pipe when no readers, and file refcount across dup/close. The
live `hello | wc` pipeline exercises blocking, EOF, and the dup2 wiring.

## Limits

Named pipes (FIFOs); multi-stage `a | b | c` (one `|` for now); `O_NONBLOCK`;
`poll`/`select`; SIGPIPE (a broken write just returns -1).
