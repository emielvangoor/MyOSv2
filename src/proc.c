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
#include "vm.h"
#include "exceptions.h"

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

// exec: replace the current process's address space with the program at `path`,
// then rewrite the trap frame so the syscall return (eret) lands in the fresh
// program. Open file descriptors are kept. Returns -1 (untouched) on failure.
int proc_exec(struct trapframe *tf, const char *path)
{
    struct file *f = vfs_open(path);
    if (!f) {
        return -1;
    }
    uint64_t len = f->vnode->size;
    uint8_t *buf = kmalloc(len ? len : 1);
    f->off = 0;
    int n = vfs_read(f, buf, len);
    vfs_close(f);
    if (n <= 0) {
        kfree(buf);
        return -1;
    }

    uint64_t entry = 0;
    struct addrspace *neu = as_create_elf(buf, (uint64_t)n, &entry);
    kfree(buf);
    if (!neu) {
        return -1;                  // not a valid ELF -> image untouched
    }

    // Install the new image and switch to it. The kernel stack we're running on
    // lives in kernel memory (not the user AS), so destroying the old AS below
    // is safe.
    struct addrspace *old = sched_current_as();
    sched_set_current_as(neu);
    as_switch(neu);

    // Make eret enter the program fresh: cleared registers, the ELF entry point,
    // a new user stack, EL0 with IRQs enabled (matching enter_user's SPSR=0).
    for (int i = 0; i < 31; i++) { tf->x[i] = 0; }
    tf->elr    = entry;
    tf->sp_el0 = USER_STACK_TOP;
    tf->spsr   = 0;

    if (old) {
        as_destroy(old);
    }
    return 0;
}
