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
#include "vfs.h"
#include "proc.h"

long do_syscall(struct trapframe *tf)
{
    uint64_t num = tf->x[8];
    long ret;

    switch (num) {
    case SYS_WRITE: {                       // x0 = fd, x1 = ptr, x2 = len
        uint64_t fd = tf->x[0];
        const char *s = (const char *)(uintptr_t)tf->x[1];
        uint64_t len = tf->x[2];
        if (fd == 1 || fd == 2) {           // stdout / stderr -> UART
            for (uint64_t i = 0; i < len; i++) { uart_putc(s[i]); }
            ret = (long)len;
        } else {
            struct file **fds = sched_current_fds();
            if (fds && fd < 16 && fds[fd]) { ret = vfs_write(fds[fd], s, len); }
            else { ret = -1; }
        }
        break;
    }
    case SYS_OPEN: {                        // x0 = path
        const char *path = (const char *)(uintptr_t)tf->x[0];
        struct file **fds = sched_current_fds();
        ret = -1;
        if (fds) {
            struct file *f = vfs_open(path);
            if (f) {
                for (int i = 3; i < 16; i++) {
                    if (!fds[i]) { fds[i] = f; ret = i; break; }
                }
                if (ret < 0) { vfs_close(f); }
            }
        }
        break;
    }
    case SYS_READ: {                        // x0 = fd, x1 = buf, x2 = len
        uint64_t fd = tf->x[0];
        void *buf = (void *)(uintptr_t)tf->x[1];
        uint64_t len = tf->x[2];
        struct file **fds = sched_current_fds();
        if (fd == 0) {                       // stdin: one char from the keyboard
            char *cb = (char *)buf;
            if (len == 0) { ret = 0; break; }
            int ch;
            while ((ch = uart_getc()) < 0) { yield(); }   // wait, letting others run
            cb[0] = (char)ch;
            ret = 1;
        } else if (fds && fd < 16 && fds[fd]) {
            ret = vfs_read(fds[fd], buf, len);
        } else { ret = -1; }
        break;
    }
    case SYS_READDIR: {                       // x0 = path, x1 = index, x2 = namebuf
        const char *path = (const char *)(uintptr_t)tf->x[0];
        int index = (int)tf->x[1];
        char *name = (char *)(uintptr_t)tf->x[2];
        struct vnode *dir = vfs_lookup(path);
        ret = dir ? vfs_readdir(dir, index, name) : -1;
        break;
    }
    case SYS_CLOSE: {                       // x0 = fd
        uint64_t fd = tf->x[0];
        struct file **fds = sched_current_fds();
        if (fds && fd < 16 && fds[fd]) { vfs_close(fds[fd]); fds[fd] = 0; ret = 0; }
        else { ret = -1; }
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
        thread_exit((int)tf->x[0]);          // x0 = exit status; does not return
        ret = 0;
        break;
    case SYS_FORK:
        ret = sched_fork(tf);                // child pid (parent); child gets 0
        break;
    case SYS_EXEC:                           // x0 = path
        ret = proc_exec(tf, (const char *)(uintptr_t)tf->x[0]);
        break;                               // on success tf is rewritten to the new image
    case SYS_WAIT:                           // x0 = int *status
        ret = sched_wait((int *)(uintptr_t)tf->x[0]);
        break;
    case SYS_SBRK:                           // x0 = signed increment
        ret = (long)as_sbrk(sched_current_as(), (long)tf->x[0]);
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
