// syscall.c -- the kernel side of the system-call interface.
// =========================================================
//
// A user (EL0) thread puts a syscall number in x8 and arguments in x0.. and
// executes `svc #0`. That traps to EL1; el0_sync_handler saves the registers as
// a trap frame and calls do_syscall(), which performs the requested service and
// writes the return value back into x0 (so `eret` hands it to the user).

#include <stdint.h>
#include "syscall.h"
#include "uart.h"
#include "sched.h"
#include "kprintf.h"

long do_syscall(struct trapframe *tf)
{
    uint64_t num = tf->x[8];
    long ret;

    switch (num) {
    case SYS_WRITE: {                       // x0 = ptr, x1 = len
        const char *s = (const char *)(uintptr_t)tf->x[0];
        uint64_t len = tf->x[1];
        for (uint64_t i = 0; i < len; i++) {
            uart_putc(s[i]);
        }
        ret = (long)len;
        break;
    }
    case SYS_GETPID:
        ret = sched_current_id();
        break;
    case SYS_YIELD:
        yield();
        ret = 0;
        break;
    case SYS_SLEEP:                          // x0 = milliseconds
        sleep_ms(tf->x[0]);
        ret = 0;
        break;
    case SYS_EXIT:
        thread_exit();                       // does not return
        ret = 0;
        break;
    case SYS_REPORT:                         // x0 = pid, x1 = value read back
        kprintf("  [user] process %d read %d  (%s)\n",
                (int)tf->x[0], (int)tf->x[1],
                (long)tf->x[0] == (long)tf->x[1] ? "ISOLATED" : "LEAKED");
        ret = 0;
        break;
    default:
        ret = -1;                            // unknown syscall
        break;
    }

    tf->x[0] = (uint64_t)ret;                // return value goes in x0
    return ret;
}
