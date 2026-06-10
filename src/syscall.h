// syscall.h -- system call numbers and the kernel-side dispatcher.
#pragma once
#include "exceptions.h"   // struct trapframe

// Syscall numbers (the user passes one in x8).
#define SYS_WRITE  0   // x0=ptr, x1=len  -> bytes written
#define SYS_GETPID 1   //                 -> current thread id
#define SYS_YIELD  2   //                 -> 0
#define SYS_SLEEP  3   // x0=ms           -> 0
#define SYS_EXIT   4   //                 -> (does not return)
#define SYS_REPORT 5   // x0=pid, x1=value -> kernel prints, returns 0
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
#define SYS_SHM_CREATE 16 // x0=len -> shared-memory handle (or -1)
#define SYS_SHM_MAP    17 // x0=handle -> base VA of the mapped object (or -1)
#define SYS_PIPE   18  // x0=int fd[2] -> fills {readfd, writefd}; 0 / -1
#define SYS_DUP2   19  // x0=old, x1=new -> new (or -1)
#define SYS_KILL   20  // x0=pid, x1=sig -> 0 / -1
#define SYS_SIGNAL 21  // x0=sig, x1=handler, x2=trampoline -> 0 / -1
#define SYS_SIGRETURN 22 // restore the pre-signal context (used by the trampoline)
#define SYS_PING   28  // x0=ip (host order), x1=int* ms -> 0 + round-trip, or -1
#define SYS_RESOLVE 29 // x0=hostname -> resolved IP (host order), or 0 on failure
#define SYS_SHUTDOWN 30 // halt the machine (does not return)
#define SYS_SOCKET   31 // x0=type -> fd
#define SYS_BIND     32 // x0=fd, x1=port -> 0/-1
#define SYS_SENDTO   33 // x0=fd, x1=buf, x2=len, x3=ip, x4=port -> bytes/-1
#define SYS_RECVFROM 34 // x0=fd, x1=buf, x2=len, x3=uint*ip, x4=uint16*port -> bytes/-1
#define SYS_CONNECT  35 // x0=fd, x1=ip, x2=port -> 0/-1 (TCP handshake)
#define SYS_LISTEN   36 // x0=fd, x1=backlog -> 0/-1 (passive open)
#define SYS_ACCEPT   37 // x0=fd -> new connected fd, or -1
#define SYS_POLL     38 // x0=pollfd*, x1=nfds, x2=timeout_ms -> #ready, 0 timeout, -1 EINTR
#define SYS_SOCKSHUT 39 // x0=fd, x1=how (SHUT_WR/...) -> 0/-1 (TCP half-close)
#define SYS_INPUT_READ 40 // x0=struct input_event* -> 0 (event) / -1 (EINTR)

// Dispatch the syscall described by the trap frame (number in x[8], args in
// x[0..]); write the result into x[0]. Returns the result too.
long do_syscall(struct trapframe *tf);
