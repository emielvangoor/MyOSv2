// syscalls.h -- system call numbers shared with the kernel (keep in sync with
// src/syscall.h). Phase 28: MyOSv2-private syscalls live at MYOS_SYS_BASE+ so
// they never collide with Linux numbers as the ABI migrates to Linux/aarch64.
#pragma once
#define MYOS_SYS_BASE 0x1000

#define SYS_READ   63
#define SYS_WRITE  64
#define SYS_CLOSE  57
#define SYS_EXIT   93
#define SYS_EXIT_GROUP 94
#define SYS_GETPID 172
#define SYS_YIELD  124
#define SYS_OPENAT 56
#define SYS_MKDIRAT 34
#define SYS_BRK    214
#define SYS_MUNMAP 215
#define SYS_MMAP   222
#define SYS_CLONE  220
#define SYS_EXECVE 221
#define SYS_WAIT4  260
#define SYS_GETCWD 17
#define SYS_CHDIR  49
#define SYS_GETDENTS64 61
#define SYS_SLEEP  3

// openat() flags + dirfd (Linux/aarch64 values).
#define AT_FDCWD   (-100)
#define O_WRONLY   01
#define O_RDWR     02
#define O_CREAT    0100
#define O_TRUNC    01000
#define SYS_READDIR 10
#define SYS_PIPE   18
#define SYS_DUP2   19
#define SYS_KILL   20
#define SYS_SIGNAL 21
#define SYS_SIGRETURN 22
#define SYS_SETPGID     44

#define SYS_REPORT      (MYOS_SYS_BASE + 0)
#define SYS_SHM_CREATE  (MYOS_SYS_BASE + 1)
#define SYS_SHM_MAP     (MYOS_SYS_BASE + 2)
#define SYS_PING        (MYOS_SYS_BASE + 3)
#define SYS_RESOLVE     (MYOS_SYS_BASE + 4)
#define SYS_SHUTDOWN    (MYOS_SYS_BASE + 5)
#define SYS_INPUT_READ  (MYOS_SYS_BASE + 6)
#define SYS_GFX_ACQUIRE (MYOS_SYS_BASE + 7)
#define SYS_GFX_FLUSH   (MYOS_SYS_BASE + 8)
#define SYS_SEAT_SWITCH (MYOS_SYS_BASE + 9)
// Socket family relocated off the Linux file-management numbers (31-39) -- see
// the long note in src/syscall.h. Native-only (musl uses Linux socket 198+).
#define SYS_SOCKET   (MYOS_SYS_BASE + 10)
#define SYS_BIND     (MYOS_SYS_BASE + 11)
#define SYS_SENDTO   (MYOS_SYS_BASE + 12)
#define SYS_RECVFROM (MYOS_SYS_BASE + 13)
#define SYS_CONNECT  (MYOS_SYS_BASE + 14)
#define SYS_LISTEN   (MYOS_SYS_BASE + 15)
#define SYS_ACCEPT   (MYOS_SYS_BASE + 16)
#define SYS_POLL     (MYOS_SYS_BASE + 17)
#define SYS_SOCKSHUT (MYOS_SYS_BASE + 18)
#define SYS_OPENPT   (MYOS_SYS_BASE + 19)
#define SYS_SET_RAWKB (MYOS_SYS_BASE + 20)
