// errno.h -- errno for native MyOSv2 programs (Phase 28). The kernel returns a
// negative errno; the ulib syscall stubs set `errno` and return -1, the C
// convention. (musl programs carry their own errno; this is for our own code.)
// Keep the values in sync with src/errno.h.
#pragma once

extern int errno;

#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define EBADF    9
#define ECHILD  10
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ESPIPE  29
#define ERANGE  34
#define ENOSYS  38
