// syscall.h -- system call numbers and the kernel-side dispatcher.
#pragma once
#include "exceptions.h"   // struct trapframe

// openat() flags + special dirfd we honor (Linux/aarch64 values).
#define AT_FDCWD   (-100)
#define O_WRONLY   01
#define O_RDWR     02
#define O_CREAT    0100
#define O_TRUNC    01000

// Syscall numbers (the user passes one in x8). Phase 28 migrates this ABI to
// Linux/aarch64 so unmodified musl binaries run. Step 1 (here): carve the
// MyOSv2-ONLY syscalls -- the ones with no Linux equivalent -- into a private
// high range (MYOS_SYS_BASE+) so they never collide with Linux numbers, which
// frees the low numbers (incl. 17/29/... that Linux uses) for the POSIX
// migration that follows. The POSIX-ish numbers below are still MyOSv2's old
// values for now; later steps give them their Linux numbers + semantics.
#define MYOS_SYS_BASE 0x1000      // MyOSv2-private syscalls live at 0x1000+

// --- migrated to Linux/aarch64 numbers + negative-errno (step 2, "clean" set,
//     identical arg registers to Linux) ---
#define SYS_READ   63  // x0=fd, x1=buf, x2=len -> bytes read / -errno
#define SYS_WRITE  64  // x0=fd, x1=ptr, x2=len -> bytes written / -errno
#define SYS_CLOSE  57  // x0=fd -> 0 / -errno
#define SYS_EXIT   93  // x0=status -> (does not return)
#define SYS_EXIT_GROUP 94 // x0=status -> (does not return; musl's _Exit/exit)
#define SYS_GETPID 172 //                 -> current thread id
#define SYS_YIELD  124 // sched_yield     -> 0

#define SYS_OPENAT 56  // x0=dirfd(AT_FDCWD), x1=path, x2=flags, x3=mode -> fd / -errno
#define SYS_IOCTL  29  // x0=fd, x1=req, x2=arg -> -ENOTTY (no real ttys yet)
#define SYS_WRITEV 66  // x0=fd, x1=struct iovec*, x2=iovcnt -> bytes / -errno
#define SYS_SET_TID_ADDRESS 96  // x0=ptr (ignored; single-threaded) -> tid
#define SYS_RT_SIGPROCMASK 135  // no per-process signal mask yet -> 0 (no-op)
#define SYS_LSEEK  62  // x0=fd, x1=offset, x2=whence -> new offset / -errno
#define SYS_UNAME  160 // x0=struct utsname* -> 0 (reports Linux/aarch64)
#define SYS_GETUID  174 // -> 0 (single-user: root)
#define SYS_GETEUID 175 // -> 0
#define SYS_GETGID  176 // -> 0
#define SYS_GETEGID 177 // -> 0
#define SYS_NEWFSTATAT 79 // x0=dirfd, x1=path, x2=statbuf, x3=flags -> 0 / -errno
#define SYS_FSTAT  80  // x0=fd, x1=statbuf -> 0 / -errno
#define SYS_BRK    214 // x0=new break (0=query) -> resulting break
#define SYS_MUNMAP 215 // x0=va, x1=len -> 0 / -errno
#define SYS_MMAP   222 // x0=addr,x1=len,... (anonymous) -> base VA / -ENOMEM

#define SYS_CLONE  220 // x0=flags(SIGCHLD), x1=child_stack -> child pid / 0 in child
#define SYS_EXECVE 221 // x0=path, x1=argv, x2=envp -> replaces image; -errno on fail
#define SYS_WAIT4  260 // x0=pid, x1=int* status, x2=options, x3=rusage -> pid / -errno

// --- still on old MyOSv2 numbers (migrate in later steps) ---
#define SYS_SLEEP  3   // x0=ms           -> 0
#define SYS_READDIR 10 // x0=path, x1=index, x2=namebuf -> 0 (name) / -1 (done)
#define SYS_PIPE   18  // x0=int fd[2] -> fills {readfd, writefd}; 0 / -1
#define SYS_DUP2   19  // x0=old, x1=new -> new (or -1)
#define SYS_KILL   20  // x0=pid, x1=sig -> 0 / -1
#define SYS_SIGNAL 21  // x0=sig, x1=handler, x2=trampoline -> 0 / -1
#define SYS_SIGRETURN 22 // restore the pre-signal context (used by the trampoline)
#define SYS_SOCKET   31 // x0=type -> fd
#define SYS_BIND     32 // x0=fd, x1=port -> 0/-1
#define SYS_SENDTO   33 // x0=fd, x1=buf, x2=len, x3=ip, x4=port -> bytes/-1
#define SYS_RECVFROM 34 // x0=fd, x1=buf, x2=len, x3=uint*ip, x4=uint16*port -> bytes/-1
#define SYS_CONNECT  35 // x0=fd, x1=ip, x2=port -> 0/-1 (TCP handshake)
#define SYS_LISTEN   36 // x0=fd, x1=backlog -> 0/-1 (passive open)
#define SYS_ACCEPT   37 // x0=fd -> new connected fd, or -1
#define SYS_POLL     38 // x0=pollfd*, x1=nfds, x2=timeout_ms -> #ready, 0 timeout, -1 EINTR
#define SYS_SOCKSHUT 39 // x0=fd, x1=how (SHUT_WR/...) -> 0/-1 (TCP half-close)
#define SYS_SETPGID     44 // x0=pid (0=self), x1=pgid (0=pid's own id) -> 0/-1

// --- MyOSv2-private syscalls (no Linux equivalent) -> high range ---
#define SYS_REPORT      (MYOS_SYS_BASE + 0)  // x0=pid, x1=value -> kernel prints
#define SYS_SHM_CREATE  (MYOS_SYS_BASE + 1)  // x0=len -> shm handle (or -1)
#define SYS_SHM_MAP     (MYOS_SYS_BASE + 2)  // x0=handle -> base VA (or -1)
#define SYS_PING        (MYOS_SYS_BASE + 3)  // x0=ip, x1=int* ms -> 0 / -1
#define SYS_RESOLVE     (MYOS_SYS_BASE + 4)  // x0=hostname -> IP (0 on failure)
#define SYS_SHUTDOWN    (MYOS_SYS_BASE + 5)  // halt the machine
#define SYS_INPUT_READ  (MYOS_SYS_BASE + 6)  // x0=struct input_event* -> 0 / -1
#define SYS_GFX_ACQUIRE (MYOS_SYS_BASE + 7)  // x0=struct gfx_info* -> 0 / -1
#define SYS_GFX_FLUSH   (MYOS_SYS_BASE + 8)  // x0=x,x1=y,x2=w,x3=h -> 0 / -1
#define SYS_SEAT_SWITCH (MYOS_SYS_BASE + 9)  // x0=seat (1-based) -> 0 / -1

// Dispatch the syscall described by the trap frame (number in x[8], args in
// x[0..]); write the result into x[0]. Returns the result too.
long do_syscall(struct trapframe *tf);
