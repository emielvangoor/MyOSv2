# Porting musl to MyOSv2 — design

**Goal:** run unmodified static aarch64 Linux binaries (built with
`aarch64-linux-musl-gcc -static`) on MyOSv2 — the same binary that runs on
Linux. North star: busybox/coreutils. First proof: a static musl `hello world`.

## Decision: MyOSv2's ABI *becomes* the Linux aarch64 ABI (Approach B)

MyOSv2 already uses the Linux register convention (number in `x8`, args
`x0–x5`, `svc #0`, result in `x0`). Approach B makes the kernel speak the Linux
**numbering and semantics** so musl "just works", rather than emulating it as a
separate per-process mode.

- **One syscall table, keyed on the Linux aarch64 number.** Standard syscalls
  use Linux numbers (write=64, read=63, openat=56, exit_group=94, clone=220,
  …), the aarch64 \*at-family (`openat`/`clone`/`dup3`/`pipe2`/`ppoll`/
  `newfstatat` — aarch64 Linux has no `open`/`fork`/`dup2`/`pipe`/`poll`/`stat`),
  and **negative-errno returns** (`−ENOENT` etc.) instead of −1.
- **MyOSv2-only syscalls move to a private high range (`0x1000+`):**
  `gfx_acquire`, `gfx_flush`, `input_read`, `seat_switch`, `shm_create`,
  `shm_map`, `ping`, `resolve`, `report`. They never collide with Linux numbers.
- **`ulib`'s C API is preserved.** `sys_open`/`fork`/`pipe`/`dup2`/`poll` keep
  their signatures; they are reimplemented over the Linux syscalls
  (`sys_open` → `openat(AT_FDCWD,…)`, `fork` → `clone(SIGCHLD)`, `dup2` →
  `dup3`, …) and the stub translates the result (`ret ∈ [−4095,−1]` →
  `errno = −ret; return −1`). So `lm_sys.c` and every native C program are
  **unchanged** — the migration is contained to `syscall.h`, `ulib.c`,
  `src/syscall.c` (dispatch), `user/crt0.S`, and `exec`'s stack builder.
- **`exec` builds the Linux initial stack:** `argc`, `argv[]`, NULL, `envp[]`,
  NULL, then **auxv** (`AT_PAGESZ`, `AT_RANDOM`, `AT_PHDR/PHENT/PHNUM`,
  `AT_ENTRY`, `AT_NULL`). Native `crt0` reads it the Linux way; musl's `_start`
  reads it natively, unchanged.

## Status

- ✅ **B-SP1** — the migration. Linux initial stack (auxv); MyOSv2-private
  syscalls at `0x1000+`; clean syscalls (`read`/`write`/`close`/`exit`/
  `getpid`/`yield`) + `openat` on Linux numbers + negative-errno; `errno` infra.
- ✅ **B-SP2** — **a static musl `hello` runs and prints** (`set_tid_address`,
  `ioctl`→`ENOTTY`, `writev`; programs linked into the clean user VA range).
- ⬜ **B-SP3** — files & processes depth (next).
- ⬜ **B-SP4** — busybox.

## Decomposition (each its own spec → plan → build)

- **B-SP1 — the migration.** Kernel dispatch + `ulib` move to Linux numbers,
  negative-errno, and \*at semantics; custom syscalls to `0x1000+`; `exec`
  builds the auxv stack; `crt0` reads it. **Success: the entire existing KTEST
  suite + all `tools/*_check.py` integration checks stay green** — MyOSv2 still
  fully works (native programs, the Lisp frame, sockets, the disk), now
  speaking Linux. No musl yet; this is the de-risked internal migration, gated
  by the tests we already have.
- **B-SP2 — static musl `hello world`.** Add the startup syscalls musl makes
  before `main`: `clone` (fork-equivalent via `SIGCHLD`), `set_tid_address`,
  `brk`/`mmap`, `rt_sigprocmask`, `writev`, `exit_group`, `ioctl`
  (`TIOCGWINSZ` → `−ENOTTY`). Build `hello.c` with `aarch64-linux-musl-gcc
  -static`, place it on the disk, run it. **Success: it prints.**
- **B-SP3 — files & processes depth.** `lseek`, `newfstatat`/`fstat`,
  `getcwd`/`chdir`, `execve`, `wait4`, `O_CREAT`/`O_TRUNC` honored in `openat`,
  `getdents64`. **Success: a real `cat`/`ls` runs.**
- **B-SP4 — busybox.** Static busybox; iterate on missing syscalls until the
  common applets run.

## Testing

TDD throughout (the standing rule). The kernel's syscall dispatch is exercised
by KTESTs that craft a trapframe and call `do_syscall` (the existing pattern in
`src/tests.c`); the user-visible behavior by the `tools/*_check.py` QEMU checks.
B-SP1's gate is "nothing regresses" — the full green suite proves the ABI swap
didn't break the working OS. B-SP2+ add a `tools/musl_check.py` that boots,
runs the musl binary from the disk, and checks its output on the serial.

## Risks

- **Big-bang dispatch renumber** (B-SP1) is mechanical but wide; the existing
  152+ KTESTs and integration checks are the safety net.
- **`clone` semantics** (B-SP2): only the `fork`-equivalent (`SIGCHLD`, no
  `CLONE_VM`) is needed first; thread-creation clones are out of scope until a
  program needs them.
- **Toolchain**: `aarch64-linux-musl-gcc` must be installed on the host (Mac);
  CI/headless builds of musl binaries depend on it.
