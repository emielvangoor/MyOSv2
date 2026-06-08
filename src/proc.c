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
// Maximum arguments we honour. The whole argv block (strings + pointer array)
// must fit in the top stack page, which limits practical sizes -- fine for a
// shell whose command line is already bounded.
#define ARGV_MAX 32
#define PAGE     4096UL

// Build the argv block on `as`'s user stack, top-down within the top stack page:
//   [ "ping\0example.com\0" ][ pad ][ argv[0], argv[1], NULL ]  <- sp
// argv[] pointers hold the *user* addresses of the strings. We write through the
// kernel's identity map (the page's physical address), so `as` need not be the
// active address space. Returns the resulting 16-byte-aligned user sp; argv[0]'s
// pointer sits exactly at that sp, so the caller passes sp as both x1 and SP.
uint64_t proc_setup_argv(struct addrspace *as, char *const argv[], int *argc_out)
{
    int argc = 0;
    if (argv) { while (argv[argc] && argc < ARGV_MAX) { argc++; } }

    uint64_t pbase = USER_STACK_TOP - PAGE;              // VA of the top stack page
    uint8_t *page  = (uint8_t *)(uintptr_t)as_translate(as, pbase);  // identity window
    // Write one byte at user VA `v` (assumed inside the top page).
    #define PUT(v, byte) (page[(uint64_t)(v) - pbase] = (uint8_t)(byte))

    uint64_t sp = USER_STACK_TOP;
    uint64_t uaddr[ARGV_MAX];
    for (int i = argc - 1; i >= 0; i--) {                // copy strings high->low
        const char *s = argv[i];
        int l = 0; while (s[l]) { l++; } l++;            // length including NUL
        sp -= (uint64_t)l;
        for (int k = 0; k < l; k++) { PUT(sp + k, s[k]); }
        uaddr[i] = sp;
    }
    sp &= ~15UL;                                         // align before the array
    sp -= (uint64_t)(argc + 1) * 8;                      // argc pointers + NULL
    sp &= ~15UL;                                         // keep sp 16-byte aligned
    for (int i = 0; i < argc; i++) {                     // little-endian pointers
        for (int b = 0; b < 8; b++) { PUT(sp + i * 8 + b, uaddr[i] >> (b * 8)); }
    }
    for (int b = 0; b < 8; b++) { PUT(sp + argc * 8 + b, 0); }   // argv[argc] = NULL
    #undef PUT

    if (argc_out) { *argc_out = argc; }
    return sp;
}

int proc_exec(struct trapframe *tf, const char *path, char *const argv[])
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

    // Copy argv onto the new program's stack BEFORE switching: the strings still
    // live in the OLD (currently active) address space, while proc_setup_argv
    // writes into `neu` through the identity map (no need for neu to be active).
    int argc = 0;
    uint64_t sp = proc_setup_argv(neu, argv, &argc);

    // Install the new image and switch to it. The kernel stack we're running on
    // lives in kernel memory (not the user AS), so destroying the old AS below
    // is safe.
    struct addrspace *old = sched_current_as();
    sched_set_current_as(neu);
    as_switch(neu);

    // Make eret enter the program fresh: cleared registers except x0/x1 which
    // carry argc/argv (the C calling convention for main), the ELF entry point,
    // the argv-topped user stack, EL0 with IRQs enabled (SPSR=0 like enter_user).
    for (int i = 0; i < 31; i++) { tf->x[i] = 0; }
    tf->x[0]   = (uint64_t)argc;
    tf->x[1]   = sp;                 // argv (== the stack pointer)
    tf->elr    = entry;
    tf->sp_el0 = sp;
    tf->spsr   = 0;

    if (old) {
        as_destroy(old);
    }
    return 0;
}
