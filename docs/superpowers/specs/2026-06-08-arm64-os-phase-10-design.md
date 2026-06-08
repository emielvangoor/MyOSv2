# MyOSv2 — Phase 10 Design (interactive shell)

**Date:** 2026-06-08
**Status:** Approved (autonomous, roadmap pre-approved)

## Goal
An interactive shell program (loaded from /bin/init) that reads a command line
from the keyboard (UART) and runs built-in commands: `help`, `echo`, `ls`, `cat`,
`exit`. Adds keyboard input (UART RX) and the `read(stdin)` / `readdir` syscalls.

Builds on Phase 7 (VFS) and Phase 8 (file syscalls, loaded programs).

## Keyboard input
- `uart_getc()` polls the PL011 receive register (FR.RXFE bit 4; read DR).
- `SYS_READ` on **fd 0** reads one byte from the UART: poll; if no byte, `yield`
  and retry (so other threads run while the shell waits). Returns 1 with the byte.

## New syscall
- `SYS_READDIR (10)` `x0=path, x1=index, x2=namebuf` -> 0 (name written) / -1 (done).
  The kernel resolves the dir vnode and calls `vfs_readdir`. Lets the shell do `ls`.

## The shell (`user/sh.c`, the new /bin/init)
Loop: print `"$ "`, read a line char-by-char (echo typed chars, handle backspace
and newline), split into a command + argument, dispatch:
- `help` — list commands.
- `echo <text>` — print the argument.
- `ls` — `SYS_READDIR("/", i, name)` for increasing i; print names.
- `cat <file>` — `open`/`read`/`write` the file to stdout.
- `exit` — `sys_exit(0)`.
- unknown — print "unknown command".

## Testing (test-first)
- `test_syscall_readdir` — `do_syscall(SYS_READDIR)` on `/` returns entry names
  (an initrd file like `hello.txt` appears), and -1 past the end. Deterministic.
- (Keyboard input + the interactive loop are observed by feeding QEMU stdin.)

## Files
`src/uart.h/.c` (uart_getc), `src/syscall.h/.c` (SYS_READ fd0, SYS_READDIR),
`user/sh.c` (+ ulib `sys_readdir`), Makefile (build sh.c as the program),
`src/tests.c` (readdir test), `docs/notes/phase-10.md`.

## Success criteria
Feeding `help`, `ls`, `cat /motd`, `exit` to the shell produces the expected
output; the readdir test passes; prior tests green; gate intact.

## Out of scope
Pipes, redirection, job control, command history, line editing beyond backspace,
running external program binaries (builtins only).
