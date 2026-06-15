// errno.h -- Linux/aarch64 errno values (Phase 28). The Linux syscall ABI
// returns errors as a NEGATIVE errno in x0 (e.g. -ENOENT = -2); userland
// (musl, or our ulib stub) turns the negative return into errno + -1. Keep in
// sync with user/errno.h.
#pragma once

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
