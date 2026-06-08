// ulib.c -- user-space syscall stubs. Number in x8, args in x0..x2, result x0.
#include "ulib.h"
#include "syscalls.h"

static long syscall3(long n, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

long sys_write(int fd, const void *b, long n) { return syscall3(SYS_WRITE, fd, (long)b, n); }
long sys_read(int fd, void *b, long n)        { return syscall3(SYS_READ, fd, (long)b, n); }
long sys_open(const char *p)                  { return syscall3(SYS_OPEN, (long)p, 0, 0); }
long sys_close(int fd)                        { return syscall3(SYS_CLOSE, fd, 0, 0); }
void sys_exit(int c)                          { syscall3(SYS_EXIT, c, 0, 0); }
long sys_getpid(void)                         { return syscall3(SYS_GETPID, 0, 0, 0); }
void sys_sleep(long ms)                       { syscall3(SYS_SLEEP, ms, 0, 0); }
long sys_fork(void)                           { return syscall3(SYS_FORK, 0, 0, 0); }
long sys_exec(const char *p)                  { return syscall3(SYS_EXEC, (long)p, 0, 0); }
long sys_wait(int *status)                    { return syscall3(SYS_WAIT, (long)status, 0, 0); }
void *sys_sbrk(long incr)                     { return (void *)syscall3(SYS_SBRK, incr, 0, 0); }
void *mmap(unsigned long len)                 { return (void *)syscall3(SYS_MMAP, (long)len, 0, 0); }
int   munmap(void *a, unsigned long len)      { return (int)syscall3(SYS_MUNMAP, (long)a, (long)len, 0); }
int   shm_create(unsigned long len)           { return (int)syscall3(SYS_SHM_CREATE, (long)len, 0, 0); }
void *shm_map(int handle)                     { return (void *)syscall3(SYS_SHM_MAP, handle, 0, 0); }
long ustrlen(const char *s) { long n = 0; while (s[n]) n++; return n; }

// --- minimal user-space malloc: a first-fit free list over sbrk ---
// Each block is [header | payload]; the header records the payload size and the
// free-list link. free() pushes a block back; malloc() reuses a fitting one or
// grows the heap with sbrk.
struct mhdr { unsigned long size; struct mhdr *next; };
static struct mhdr *mfree;

void *malloc(unsigned long n)
{
    n = (n + 15) & ~15UL;                       // 16-byte align the payload
    struct mhdr **pp = &mfree;
    for (struct mhdr *b = mfree; b; pp = &b->next, b = b->next) {
        if (b->size >= n) { *pp = b->next; return (void *)(b + 1); }   // reuse
    }
    struct mhdr *b = (struct mhdr *)sys_sbrk((long)(n + sizeof(struct mhdr)));
    if ((long)b == -1) { return 0; }            // out of memory
    b->size = n;
    return (void *)(b + 1);
}

void free(void *p)
{
    if (!p) { return; }
    struct mhdr *b = (struct mhdr *)p - 1;
    b->next = mfree;                            // push onto the free list
    mfree = b;
}
long sys_readdir(const char *p, int i, char *name) { return syscall3(SYS_READDIR, (long)p, i, (long)name); }
int  sys_getc(void) { char c; long n = syscall3(SYS_READ, 0, (long)&c, 1); return n == 1 ? (int)(unsigned char)c : -1; }
