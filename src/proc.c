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

// AArch64 Linux auxiliary-vector tags we provide. A static musl binary only
// really needs AT_PAGESZ and AT_RANDOM (the stack-canary seed); the rest of the
// world it discovers by syscall. (The Linux ABI; see <elf.h>.)
#define AT_NULL    0
#define AT_PAGESZ  6
#define AT_RANDOM 25

// Build the Linux/aarch64 initial process stack on `as`'s top stack page, so an
// unmodified musl `_start` (which reads everything FROM the stack) works. Layout,
// top-down, with sp pointing at argc:
//   [ strings "prog\0arg\0" ][ 16 random bytes ][ pad ]
//   sp -> [ argc ][ argv0..argv(n-1) ][ NULL ][ envp NULL ][ auxv... ][ AT_NULL ]
// We also return argc and the argv-array address so the caller can keep loading
// x0/x1 for our OWN crt0 (which still reads args from registers) -- both ABIs are
// satisfied at once. Written through the kernel identity map, so `as` need not be
// active. The returned sp is 16-byte aligned (the ABI requires it at _start).
uint64_t proc_setup_argv(struct addrspace *as, char *const argv[],
                         int *argc_out, uint64_t *argv_out)
{
    int argc = 0;
    if (argv) { while (argv[argc] && argc < ARGV_MAX) { argc++; } }

    uint64_t pbase = USER_STACK_TOP - PAGE;              // VA of the top stack page
    uint8_t *page  = (uint8_t *)(uintptr_t)as_translate(as, pbase);  // identity window
    #define PUT(v, byte) (page[(uint64_t)(v) - pbase] = (uint8_t)(byte))
    #define PUT64(v, val) do { uint64_t _x = (val); \
        for (int _b = 0; _b < 8; _b++) { PUT((v) + _b, _x >> (_b * 8)); } } while (0)

    uint64_t sp = USER_STACK_TOP;
    uint64_t uaddr[ARGV_MAX];
    for (int i = argc - 1; i >= 0; i--) {                // copy strings high->low
        const char *s = argv[i];
        int l = 0; while (s[l]) { l++; } l++;            // length including NUL
        sp -= (uint64_t)l;
        for (int k = 0; k < l; k++) { PUT(sp + k, s[k]); }
        uaddr[i] = sp;
    }
    // 16 bytes for AT_RANDOM (musl's stack canary). Not crypto -- a varying
    // pattern is enough for a hobby OS; a real RNG can replace it later.
    static uint64_t rseed = 0x9e3779b97f4a7c15UL;
    sp -= 16;
    uint64_t rnd = sp;
    for (int b = 0; b < 16; b++) { rseed = rseed * 6364136223846793005UL + 1; PUT(rnd + b, rseed >> 56); }

    // The vector: argc + argv[]+NULL + envp[NULL] + 3 auxv pairs. Size it, then
    // drop sp so that argc lands 16-byte aligned.
    int words = 1 + (argc + 1) + 1 + 6;                  // argc, argv+NULL, envp NULL, 3 auxv pairs
    sp -= (uint64_t)words * 8;
    sp &= ~15UL;

    uint64_t p = sp;
    PUT64(p, (uint64_t)argc); p += 8;                    // argc
    uint64_t argv_arr = p;
    for (int i = 0; i < argc; i++) { PUT64(p, uaddr[i]); p += 8; }
    PUT64(p, 0); p += 8;                                 // argv[argc] = NULL
    PUT64(p, 0); p += 8;                                 // envp[0] = NULL (empty env)
    PUT64(p, AT_PAGESZ); p += 8; PUT64(p, PAGE);     p += 8;
    PUT64(p, AT_RANDOM); p += 8; PUT64(p, rnd);      p += 8;
    PUT64(p, AT_NULL);   p += 8; PUT64(p, 0);        p += 8;
    #undef PUT
    #undef PUT64

    if (argc_out) { *argc_out = argc; }
    if (argv_out) { *argv_out = argv_arr; }
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
    uint64_t argv_arr = 0;
    uint64_t sp = proc_setup_argv(neu, argv, &argc, &argv_arr);

    // Install the new image and switch to it. The kernel stack we're running on
    // lives in kernel memory (not the user AS), so destroying the old AS below
    // is safe.
    struct addrspace *old = sched_current_as();
    sched_set_current_as(neu);
    as_switch(neu);

    // Make eret enter the program fresh: cleared registers, the ELF entry point,
    // the argv-topped user stack, EL0 with IRQs enabled (SPSR=0 like enter_user).
    // x1 carries argv; x0 (argc) is set by do_syscall from our return value --
    // it writes tf->x[0] = ret after we return, so returning argc lands it in x0.
    for (int i = 0; i < 31; i++) { tf->x[i] = 0; }
    tf->x[1]   = argv_arr;           // argv array, for our crt0's register ABI
    tf->elr    = entry;
    tf->sp_el0 = sp;                 // the full Linux stack (argc word) for musl
    tf->spsr   = 0;

    if (old) {
        as_destroy(old);
    }
    return argc;                     // -> x0 = argc (the C entry convention)
}
