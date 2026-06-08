# Phase 8 notes — exec + file syscalls

## What changed

MyOSv2 runs **programs loaded from the filesystem**, with file descriptors.

```
loaded program says hello (running at EL0)   (greeting via write syscall)
Welcome to MyOSv2.                            (/motd opened + read + printed)
```

## The user-program build pipeline

User programs are **separate flat binaries linked at USER_CODE_VA** (0x8000000000),
so every address (code AND string literals) is correct at the run address with no
relocation. (An earlier attempt to embed kernel-compiled code as a
position-independent blob failed: string pointers landed outside the copied image.)

- `user/crt0.S` `_start` -> `umain()` -> `sys_exit` (entry at offset 0).
- `user/ulib.c` syscall stubs; `user/prog.c` the program; `user/user.ld` links at
  USER_CODE_VA.
- Makefile: compile -> link with `user.ld` -> `objcopy -O binary` -> `xxd -i` into
  `build/user_blob.c` (`init_bin[]`, `init_bin_len`) -> compiled into the kernel.
- The initrd registers `init_bin` as `/bin/init`.

## Loading (`as_create_image`, `proc_spawn`)

- `as_create_image(img, len)` builds an address space whose private code pages
  hold the image (mapped RO, EL0-exec at USER_CODE_VA); private stack + data.
- `proc_spawn(path)` reads the whole program file into a buffer and
  `thread_create_image`s it. The program could come from any filesystem.

## File descriptors + syscalls

Per-thread `fds[16]` (a process = a user thread). fds 0/1/2 = console. New syscalls:
`SYS_OPEN` (path -> fd >= 3), `SYS_READ` (fd,buf,len), `SYS_CLOSE` (fd); `SYS_WRITE`
gained an fd argument (1/2 -> UART, fd >= 3 -> vfs_write). The kernel reads user
pointers through the currently-active address space (mapped EL0-accessible, so EL1
can read them). `do_syscall` reaches the fd table via `sched_current_fds()`.

## Testing

`as_create_image` mapping + 4 fd tests (open returns fd, read, open-missing,
close-then-reuse), run inside worker threads so `current` has an fd table.
Test-first. Running the loaded EL0 program is observed in the demo.

## Limits

One embedded program; entry assumed at offset 0 (crt0); no ELF parsing (flat
binary); no argv/env; no real stdin yet (Phase 10). exec here is "spawn"; true
image replacement pairs with fork (Phase 9).
