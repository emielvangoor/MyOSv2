// syscall.h -- system call numbers and the kernel-side dispatcher.
#pragma once
#include "exceptions.h"   // struct trapframe

// Syscall numbers (the user passes one in x8).
#define SYS_WRITE  0   // x0=ptr, x1=len  -> bytes written
#define SYS_GETPID 1   //                 -> current thread id
#define SYS_YIELD  2   //                 -> 0
#define SYS_SLEEP  3   // x0=ms           -> 0
#define SYS_EXIT   4   //                 -> (does not return)

// Dispatch the syscall described by the trap frame (number in x[8], args in
// x[0..]); write the result into x[0]. Returns the result too.
long do_syscall(struct trapframe *tf);
