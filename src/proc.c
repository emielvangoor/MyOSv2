// proc.c -- load a program image from a file and run it as a new process.
// =======================================================================
//
// This is "exec" in the spawn sense: read the whole program file into a kernel
// buffer, then build a fresh address space from that image and start a user
// thread in it. The program could come from any filesystem -- ramfs today, a
// real disk later -- because it's reached through the VFS.

#include <stdint.h>
#include "proc.h"
#include "vfs.h"
#include "sched.h"
#include "kheap.h"

struct thread *proc_spawn(const char *path, int priority)
{
    struct file *f = vfs_open(path);
    if (!f) {
        return 0;
    }
    uint64_t len = f->vnode->size;
    uint8_t *buf = kmalloc(len ? len : 1);
    f->off = 0;
    int n = vfs_read(f, buf, len);
    vfs_close(f);
    if (n <= 0) {
        return 0;
    }
    return thread_create_image(buf, (uint64_t)n, priority);
}
