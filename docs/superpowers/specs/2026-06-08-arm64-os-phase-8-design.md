# MyOSv2 — Phase 8 Design (exec + file syscalls)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Load and run a program **from the filesystem**, and add **file descriptors** with
`open`/`read`/`close` syscalls (and fd-aware `write`). The program image is read
from a file and mapped into a fresh address space — a real exec pipeline that
would work identically if the file came from a real disk.

Builds on Phase 6b (per-process address spaces) and Phase 7 (VFS/ramfs/initrd).
Low-risk: reuses the working address-space loader, parameterized by image.

## The program image (no new build system)

The user program already lives in the kernel's `.user` linker section
(`__user_start`..`__user_end`, position-independent, runs at `USER_CODE_VA`). We
register that blob as an **initrd file** `/bin/init`, so it can be loaded through
the normal file path. (A real on-disk binary would slot in the same way; we avoid
a separate toolchain build for now.)

To let user programs use string literals, string constants are placed in `.user`
too (via `__attribute__((section(".user")))`), so code + data are one contiguous,
relocatable blob.

## Loader (`vm.c`, `proc.c`)

- `as_create_image(const void *img, uint64_t len)` — generalize the Phase 6b
  `as_create`: allocate fresh code pages, copy `img` into them, map read-only
  EL0-exec at `USER_CODE_VA`; private stack + data pages as before. (Each loaded
  program gets its own code copy — simple and fully isolated.)
- `thread_create_image(const void *img, uint64_t len, int priority)` — a user
  thread whose address space is `as_create_image(img,len)`, entering at
  `USER_CODE_VA`.
- `proc_spawn(const char *path, int priority)` — `vfs_open(path)`, read the whole
  file into a `kmalloc` buffer, `thread_create_image(buf, len, priority)`. Returns
  the new thread (or NULL).

`vm_init()` and the global shared-code copy from 6b are removed; `thread_create_user`
is replaced by the image-based path.

## File descriptors + syscalls

Per-thread fd table (a process = a user thread here):
```c
#define MAX_FDS 16
struct file *fds[MAX_FDS];   // in struct thread; fd = index; NULL = free
```
fds 0/1/2 are the console (write to 1/2 → UART; read from 0 → 0 bytes for now).

Syscalls (numbers continue from Phase 6/7; `SYS_REPORT` was 5):
- `SYS_OPEN (6)`  `x0=path` → fd ≥ 3, or -1. The kernel reads the user path through
  the current address space (mapped EL0-accessible, so EL1 can read it).
- `SYS_READ (7)`  `x0=fd, x1=buf, x2=len` → bytes read (fd ≥ 3 → `vfs_read`).
- `SYS_CLOSE (8)` `x0=fd` → 0.
- `SYS_WRITE` extended: `x0=fd` (1/2 → UART as today; fd ≥ 3 → `vfs_write`).

`do_syscall` reaches the current thread's fd table via a `sched_current_fds()`
accessor.

## Demo (`kmain`)

Mount FS + initrd (now also `/bin/init`), then `proc_spawn("/bin/init", 2)` plus a
kernel thread. The loaded program prints a greeting (write syscall), `open`s
`/motd` and `read`s + prints it, then exits — a program **loaded from a file**
doing file I/O via syscalls.

## Testing (test-first, deterministic, in-kernel)

1. `test_as_image_maps_code` — `as_create_image(buf,len)` maps `USER_CODE_VA`, and
   the bytes at that physical page equal `buf` (via `as_translate`).
2. `test_fd_open_returns_fd` — in a worker thread, `do_syscall(SYS_OPEN)` on an
   initrd file returns a fd ≥ 3.
3. `test_fd_read_syscall` — `SYS_READ` on that fd returns the file's bytes.
4. `test_fd_open_missing` — `SYS_OPEN` of a missing path returns -1.
5. `test_fd_close_reuse` — after `SYS_CLOSE`, the next `SYS_OPEN` reuses the fd.

(Running the loaded EL0 program is observed in the demo.) These tests run inside a
worker thread so `current` has an fd table; each mounts a fresh ramfs + initrd.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/vm.h`/`vm.c` | `as_create_image` (image-parameterized) |
| `src/proc.h`/`proc.c` | `proc_spawn(path, prio)` |
| `src/sched.h`/`sched.c` | per-thread `fds[]`; `thread_create_image`; `sched_current_fds` |
| `src/syscall.h`/`syscall.c` | `SYS_OPEN/READ/CLOSE`; fd-aware `write` |
| `src/initrd.c` | register the `.user` blob as `/bin/init` |
| `src/user.c` | program uses strings (in `.user`); does file I/O |
| `src/tests.c` | 5 tests (test-first) |
| `src/kmain.c` | `proc_spawn("/bin/init")` |
| `docs/notes/phase-8.md` | notes |

## Success criteria

- 5 new tests pass (test-first); the demo runs a program **loaded from `/bin/init`**
  that writes and reads a file via syscalls; prior tests stay green; gate intact.

## Out of scope

`fork`/COW (9), shell (10), ASIDs (11); a separate user toolchain build; argv/env;
real stdin (Phase 10 adds UART input); multiple distinct on-disk binaries.
