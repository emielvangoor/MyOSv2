// user.c -- a string-free EL0 program placed in the .user section so it can be
// copied into a user address space and run there. It writes its pid to its
// PRIVATE data page, sleeps, reads it back, and reports -- proving isolation.
//
// It must be self-contained (only its own functions + svc) and string-literal
// free (no .rodata), so the contiguous .user blob is correct when relocated to
// the user code virtual address and executed with PC-relative addressing.
#include <stdint.h>
#include "user.h"
#include "syscall.h"
#include "vm.h"   // USER_DATA_VA

#define USER __attribute__((section(".user")))

// The EL0 system-call stub: number in x8, args in x0/x1, result in x0.
USER static long syscall(long num, long a0, long a1)
{
    register long x8 __asm__("x8") = num;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

USER void user_main(void)
{
    long pid = syscall(SYS_GETPID, 0, 0);

    volatile long *data = (volatile long *)USER_DATA_VA;
    *data = pid;                       // write our pid to our PRIVATE data page
    syscall(SYS_SLEEP, 150, 0);        // the other process runs meanwhile
    long seen = *data;                 // read back

    syscall(SYS_REPORT, pid, seen);    // kernel prints; isolated => seen == pid
    syscall(SYS_EXIT, 0, 0);           // never returns
}
