# Phase 10 notes — interactive shell

## What changed
A real interactive shell, loaded from /bin/init, reading the keyboard:

```
MyOSv2 shell. Type 'help'.
$ help
commands: help echo ls cat <f> exit
$ ls
tmp.txt  bin  motd  hello.txt
$ cat /motd
Welcome to MyOSv2.
$ exit
```

## Keyboard input
`uart_getc()` polls the PL011 receive register (FR.RXFE; read DR). `SYS_READ` on
fd 0 returns one keystroke, `yield`-ing while the FIFO is empty so other threads
run while the shell waits for input.

## SYS_READDIR
`SYS_READDIR(path, index, namebuf)` resolves the directory vnode and calls
`vfs_readdir` -- lets the shell implement `ls`.

## The shell (user/sh.c)
Built-in commands only (busybox-style): `help`, `echo`, `ls`, `cat`, `exit`. It
reads a line (echoing keystrokes, handling backspace + enter), splits command +
argument, and dispatches -- all through syscalls (open/read/write/close/readdir).

## Testing
`SYS_READDIR` is unit-tested (returns entry names for `/`, -1 past the end). The
interactive loop + keyboard input are observed by piping commands to QEMU's stdin.

## Limits
Built-ins only (no external binaries -- would need argv + multiple program
images); no pipes/redirection/history; backspace is the only line editing.
