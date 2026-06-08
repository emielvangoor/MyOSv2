# MyOSv2 — Phase 17 Design (pipes)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

A **pipe**: a unidirectional in-kernel byte channel with two ends (a read fd and
a write fd). `read` blocks until data arrives or all writers close (EOF); `write`
blocks until space frees or all readers close (broken pipe). With `dup2`, the
shell wires one command's stdout to the next's stdin — real pipelines like
`hello | wc`.

Builds on the fd table (8), the process lifecycle (13), and the VFS file layer.

## Pipe object (`pipe.c`)

```c
#define PIPE_SIZE 4096
struct pipe {
    char     buf[PIPE_SIZE];
    uint32_t r, w;        // ring read/write positions
    uint32_t count;       // bytes currently buffered
    int      readers;     // open read ends
    int      writers;     // open write ends
};
```
- `pipe_read(f, buf, len)`: while `count==0 && writers>0` → `yield()` (block);
  if still empty → return 0 (**EOF**); else copy `min(len, count)` bytes out.
- `pipe_write(f, buf, len)`: copy as space allows; while full and `readers>0` →
  `yield()`; if `readers==0` → return bytes-so-far or -1 (**broken pipe**).
- `pipe_close(f)`: decrement `readers`/`writers` by which end; free the object
  when both reach 0.

## Integrating with `struct file`

`struct file` gains a pipe pointer, a direction, and a **reference count** (fds
are shared across `fork`/`dup`, so a file must outlive any single close):
```c
struct file {
    struct vnode *vnode;   // NULL for a pipe end
    uint64_t      off;
    struct pipe  *pipe;    // non-NULL => this is a pipe end
    int           writable;// pipe direction: 1 = write end
    int           ref;     // open references
};
```
- `vfs_open` sets `ref=1`, `pipe=NULL`. `vfs_read`/`vfs_write` route to
  `pipe_read`/`pipe_write` when `pipe != NULL`, else to the vnode ops.
- `vfs_close` does `if (--ref == 0) { if (pipe) pipe_close(f); kfree(f); }`.
- `file_dup(f)` does `ref++` and returns `f`.
- `sched_fork` `file_dup`s every inherited fd (parent and child share each open
  file with a correct count) — fixing a latent fork/close double-free.

## fd routing (`syscall.c`)

Today `SYS_WRITE` sends fd 1/2 straight to the UART and `SYS_READ` reads fd 0 from
the keyboard. Change to **prefer an open file**, falling back to the console only
when the slot is empty:
- `SYS_WRITE(fd)`: if `fds[fd]` → `vfs_write`; else if fd 1/2 → UART; else -1.
- `SYS_READ(fd)`: if `fds[fd]` → `vfs_read`; else if fd 0 → keyboard; else -1.

So a pipe end `dup2`'d onto fd 0/1 is used for I/O; the bare shell (fds 0–2 unset)
still talks to the console.

## Syscalls

```
SYS_PIPE = 18   // x0 = int fd[2]  -> fills {readfd, writefd}; returns 0 / -1
SYS_DUP2 = 19   // x0 = oldfd, x1 = newfd -> newfd (or -1)
```
`SYS_PIPE` allocates a `struct pipe` (readers=writers=1) and two files (a read end
and a write end), installs them in the two lowest free fds, and writes the numbers
to the user array. `SYS_DUP2` closes `newfd` if open, then points it at `fds[oldfd]`
(`file_dup`).

## A filter program + the shell

`/bin/wc`: read stdin to EOF, print the byte count, exit 0 — a real stdin→stdout
filter. The shell parses a single `|`: for `A | B` it creates a pipe, forks A with
its stdout `dup2`'d to the write end, forks B with its stdin `dup2`'d to the read
end, closes both pipe fds in every process that shouldn't hold them, and waits.
`hello | wc` then prints `22` (the length of hello's output).

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/pipe.h`/`pipe.c` | `struct pipe`, `pipe_alloc`/`read`/`write`/`close` |
| `src/vfs.h`/`vfs.c` | `struct file` (pipe/ref fields), routing, `file_dup`, refcounted close |
| `src/sched.c` | `sched_fork` increfs inherited fds |
| `src/syscall.h`/`syscall.c` | `SYS_PIPE`, `SYS_DUP2`, fd-first routing |
| `user/syscalls.h`, `user/ulib.*` | `pipe`, `dup2` wrappers |
| `user/wc.c` | stdin→count filter |
| `user/sh.c` | parse and run `A | B` |
| `Makefile`, `src/initrd.c` | build + register `/bin/wc` |
| `src/tests.c` | pipe + refcount tests (test-first) |
| `docs/notes/phase-17.md` | notes |

## Testing (test-first, kernel-level)

The ring buffer + EOF/broken semantics are unit-tested without relying on
blocking (conditions are set so no `yield` is needed); the live pipeline exercises
blocking.

1. `test_pipe_write_then_read` — write "hello" (5) then read 5 → "hello",
   `count` back to 0.
2. `test_pipe_wraps` — fill near `PIPE_SIZE`, read some, write more (ring
   wrap-around) — bytes come out in order.
3. `test_pipe_eof` — `writers=0`, `count=0`: `pipe_read` returns 0.
4. `test_pipe_broken` — `readers=0`, full: `pipe_write` returns -1.
5. `test_file_refcount` — `file_dup` then `vfs_close` once keeps the file
   (a second read still works); a final close frees it. (Use a ramfs-backed file
   and check via a sentinel / no crash; assert `ref` transitions.)

## Success criteria

- 5 tests pass (test-first); prior tests stay green; gate holds.
- Live: `hello | wc` prints `22`; a bare command and the interactive shell still
  use the console; repeated pipelines don't leak (refcounted close).

## Out of scope

Named pipes (FIFOs); multi-stage pipelines `a | b | c` (one `|` for now);
`O_NONBLOCK`; `poll`/`select`; SIGPIPE (a broken write just returns -1).
