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

// Dispatch the syscall described by the trap frame (number in x[8], args in
// x[0..]); write the result into x[0]. Returns the result too.
long do_syscall(struct trapframe *tf);
