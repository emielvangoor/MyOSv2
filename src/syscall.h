// syscall.h -- system call numbers and the kernel-side dispatcher.
#pragma once
#include "exceptions.h"   // struct trapframe

// Syscall numbers (the user passes one in x8). Phase 28 migrates this ABI to
// Linux/aarch64 so unmodified musl binaries run. Step 1 (here): carve the
// MyOSv2-ONLY syscalls -- the ones with no Linux equivalent -- into a private
// high range (MYOS_SYS_BASE+) so they never collide with Linux numbers, which
// frees the low numbers (incl. 17/29/... that Linux uses) for the POSIX
// migration that follows. The POSIX-ish numbers below are still MyOSv2's old
// values for now; later steps give them their Linux numbers + semantics.
#define MYOS_SYS_BASE 0x1000      // MyOSv2-private syscalls live at 0x1000+

#define SYS_WRITE  0   // x0=fd, x1=ptr, x2=len -> bytes written
#define SYS_GETPID 1   //                 -> current thread id
#define SYS_YIELD  2   //                 -> 0
#define SYS_SLEEP  3   // x0=ms           -> 0
#define SYS_EXIT   4   //                 -> (does not return)
#define SYS_OPEN   6   // x0=path -> fd (>=3) or -1
#define SYS_READ   7   // x0=fd, x1=buf, x2=len -> bytes read
#define SYS_CLOSE  8   // x0=fd -> 0
#define SYS_FORK   9   // -> child pid in parent, 0 in child
#define SYS_READDIR 10 // x0=path, x1=index, x2=namebuf -> 0 (name) / -1 (done)
#define SYS_EXEC   11  // x0=path -> replaces image; returns -1 only on failure
#define SYS_WAIT   12  // x0=int* status -> reaped child pid, or -1 if no children
#define SYS_SBRK   13  // x0=signed increment -> previous program break (or -1)
#define SYS_MMAP   14  // x0=len -> base VA of a fresh anonymous region (or -1)
#define SYS_MUNMAP 15  // x0=va, x1=len -> 0 / -1
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
