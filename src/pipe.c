// pipe.c -- the pipe ring buffer with blocking read/write.
// ========================================================
//
// Blocking uses the V6 sleep/wakeup primitive: a reader that finds the buffer
// empty (but writers still exist) SLEEPS on the pipe (sched_block) until a writer
// feeds it and wakes it (sched_wake); likewise a full-buffer writer sleeps until
// a reader drains it. The pipe's address is the wait-channel. Sleeping (not
// yield-spinning) is essential: a spinning reader runs inside a syscall with IRQs
// masked, and if it's the only runnable thread it starves the timer/device
// interrupts that a sleeping peer (e.g. a thread waiting on the network) needs to
// wake -- a deadlock. A sleeper yields the CPU to idle, so interrupts flow.

#include <stdint.h>
#include "pipe.h"
#include "vfs.h"
#include "kheap.h"
#include "sched.h"

struct pipe *pipe_alloc(void)
{
    struct pipe *p = kmalloc(sizeof(struct pipe));
    p->r = p->w = p->count = 0;
    p->readers = 1;
    p->writers = 1;
    return p;
}

int pipe_read(struct file *f, void *buf, uint64_t len)
{
    struct pipe *p = f->pipe;
    while (p->count == 0 && p->writers > 0) {
        sched_block(p);                  // sleep until a writer feeds us (or all close)
    }
    if (p->count == 0) {
        return 0;                        // empty and no writers left -> EOF
    }
    char *b = (char *)buf;
    uint64_t n = 0;
    while (n < len && p->count > 0) {
        b[n++] = p->buf[p->r];
        p->r = (p->r + 1) % PIPE_SIZE;
        p->count--;
    }
    sched_wake(p);                       // space freed -> wake a blocked writer
    return (int)n;
}

int pipe_write(struct file *f, const void *buf, uint64_t len)
{
    struct pipe *p = f->pipe;
    const char *b = (const char *)buf;
    uint64_t n = 0;
    while (n < len) {
        while (p->count == PIPE_SIZE && p->readers > 0) {
            sched_block(p);              // sleep until the reader drains the buffer
        }
        if (p->readers == 0) {
            return n ? (int)n : -1;      // nobody to read -> broken pipe
        }
        while (n < len && p->count < PIPE_SIZE) {
            p->buf[p->w] = b[n++];
            p->w = (p->w + 1) % PIPE_SIZE;
            p->count++;
        }
        sched_wake(p);                   // data available -> wake a blocked reader
    }
    return (int)n;
}

void pipe_close(struct file *f)
{
    struct pipe *p = f->pipe;
    if (f->writable) { p->writers--; } else { p->readers--; }
    sched_wake(p);                       // let a blocked peer notice the closed end
    if (p->readers == 0 && p->writers == 0) {
        kfree(p);                        // last end closed -> free the buffer
    }
}
