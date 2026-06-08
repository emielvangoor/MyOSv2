// pipe.c -- the pipe ring buffer with blocking read/write.
// ========================================================
//
// Blocking is cooperative: when a reader finds the buffer empty (but writers
// still exist) it yield()s the CPU and re-checks; likewise a writer on a full
// buffer. This is simple and correct on our single-core scheduler -- the other
// end runs, changes `count`, and the loop makes progress.

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
        yield();                         // wait for a writer (or for them all to close)
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
    return (int)n;
}

int pipe_write(struct file *f, const void *buf, uint64_t len)
{
    struct pipe *p = f->pipe;
    const char *b = (const char *)buf;
    uint64_t n = 0;
    while (n < len) {
        while (p->count == PIPE_SIZE && p->readers > 0) {
            yield();                     // wait for the reader to drain the buffer
        }
        if (p->readers == 0) {
            return n ? (int)n : -1;      // nobody to read -> broken pipe
        }
        while (n < len && p->count < PIPE_SIZE) {
            p->buf[p->w] = b[n++];
            p->w = (p->w + 1) % PIPE_SIZE;
            p->count++;
        }
    }
    return (int)n;
}

void pipe_close(struct file *f)
{
    struct pipe *p = f->pipe;
    if (f->writable) { p->writers--; } else { p->readers--; }
    if (p->readers == 0 && p->writers == 0) {
        kfree(p);                        // last end closed -> free the buffer
    }
}
