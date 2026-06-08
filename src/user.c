// user.c -- a tiny program that runs at EL0 (user mode).
// =====================================================
//
// This code executes UNPRIVILEGED. It cannot touch hardware or kernel-only
// memory directly; its only way to ask the kernel for anything is the `svc`
// trap, made through the syscall() stub below. (Compiled into the kernel image
// for Phase 6a and run via the EL0 RAM alias; Phase 6b loads it into a private
// per-process address space.)
//
// It must stay self-contained: it may only call its own functions and `svc`.
// The compiler emits PC-relative code, so it runs correctly at the alias address.

#include <stdint.h>
#include "user.h"
#include "syscall.h"

// The EL0 system-call stub: number in x8, args in x0/x1, result returned in x0.
static long syscall(long num, long a0, long a1)
{
    register long x8 __asm__("x8") = num;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1)
                     : "memory");
    return x0;
}

static long ustrlen(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void user_main(void)
{
    const char *hello = "Hello from EL0 user mode!\n";
    syscall(SYS_WRITE, (long)(uintptr_t)hello, ustrlen(hello));

    syscall(SYS_GETPID, 0, 0);     // proves the call works (pid not printed here)
    syscall(SYS_SLEEP, 100, 0);    // block ~100 ms so the kernel thread runs

    const char *bye = "user thread woke, exiting via syscall.\n";
    syscall(SYS_WRITE, (long)(uintptr_t)bye, ustrlen(bye));

    syscall(SYS_EXIT, 0, 0);       // never returns
}
