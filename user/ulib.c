// ulib.c -- user-space syscall stubs. Number in x8, args in x0..x2, result x0.
#include "ulib.h"
#include "syscalls.h"
#include "errno.h"

int errno;

// Linux syscalls report errors as a negative errno in x0. Turn that into the
// C convention (set errno, return -1) for the syscalls we've migrated; a normal
// (non-negative, or far-negative pointer) result passes through unchanged.
static long sysret(long r)
{
    if (r < 0 && r > -4096) { errno = (int)(-r); return -1; }
    return r;
}

static long syscall3(long n, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

// Five-argument variant (sendto/recvfrom need more than three).
static long syscall5(long n, long a0, long a1, long a2, long a3, long a4)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

long sys_write(int fd, const void *b, long n) { return sysret(syscall3(SYS_WRITE, fd, (long)b, n)); }
long sys_read(int fd, void *b, long n)        { return sysret(syscall3(SYS_READ, fd, (long)b, n)); }
long sys_open(const char *p)                  { return sysret(syscall5(SYS_OPENAT, AT_FDCWD, (long)p, 0, 0, 0)); }
long sys_creat(const char *p)                 { return sysret(syscall5(SYS_OPENAT, AT_FDCWD, (long)p, O_CREAT|O_WRONLY|O_TRUNC, 0666, 0)); }
long sys_mkdir(const char *p)                 { return sysret(syscall5(SYS_MKDIRAT, AT_FDCWD, (long)p, 0777, 0, 0)); }
long sys_close(int fd)                        { return sysret(syscall3(SYS_CLOSE, fd, 0, 0)); }
void sys_exit(int c)                          { syscall3(SYS_EXIT, c, 0, 0); }
long sys_getpid(void)                         { return syscall3(SYS_GETPID, 0, 0, 0); }
void sys_sleep(long ms)                       { syscall3(SYS_SLEEP, ms, 0, 0); }
long sys_fork(void)                           { return syscall3(SYS_CLONE, 17 /*SIGCHLD*/, 0, 0); }
long sys_exec(const char *p, char *const argv[]) { return syscall3(SYS_EXECVE, (long)p, (long)argv, 0); }
long sys_wait(int *status)                    {           // decode Linux status -> raw
    int st = 0;
    long pid = syscall5(SYS_WAIT4, -1, (long)&st, 0, 0, 0);
    if (status) { *status = (st >> 8) & 0xff; }          // native callers want the raw code
    return pid;
}
void *sys_sbrk(long incr)                     {           // sbrk over Linux brk
    long cur = syscall3(SYS_BRK, 0, 0, 0);                // query current break
    if (incr == 0) { return (void *)cur; }
    long neu = syscall3(SYS_BRK, cur + incr, 0, 0);
    return (neu == cur + incr) ? (void *)cur : (void *)-1;  // sbrk returns the OLD break
}
void *mmap(unsigned long len)                 {           // anonymous: kernel reads x1=len
    long r = syscall5(SYS_MMAP, 0, (long)len, 3 /*PROT_RW*/, 0x22 /*ANON|PRIVATE*/, -1);
    return (r < 0 && r > -4096) ? (void *)-1 : (void *)r;  // -errno -> MAP_FAILED
}
int   munmap(void *a, unsigned long len)      { return (int)syscall3(SYS_MUNMAP, (long)a, (long)len, 0); }
int   shm_create(unsigned long len)           { return (int)syscall3(SYS_SHM_CREATE, (long)len, 0, 0); }
void *shm_map(int handle)                     { return (void *)syscall3(SYS_SHM_MAP, handle, 0, 0); }
int   pipe(int fd[2])                         { return (int)syscall3(SYS_PIPE, (long)fd, 0, 0); }
int   dup2(int o, int n)                      { return (int)syscall3(SYS_DUP2, o, n, 0); }
int   kill(int pid, int sig)                  { return (int)syscall3(SYS_KILL, pid, sig, 0); }
int   setpgid(int pid, int pgid)              { return (int)syscall3(SYS_SETPGID, pid, pgid, 0); }

// A signal handler returns into this stub, which asks the kernel to restore the
// pre-signal context.
void __sigreturn(void) { syscall3(SYS_SIGRETURN, 0, 0, 0); }

int signal(int sig, void (*handler)(int))
{
    return (int)syscall3(SYS_SIGNAL, sig, (long)handler, (long)__sigreturn);
}

int ping(unsigned int ip, int *ms) { return (int)syscall3(SYS_PING, ip, (long)ms, 0); }
unsigned int resolve(const char *host) { return (unsigned int)syscall3(SYS_RESOLVE, (long)host, 0, 0); }
void shutdown(void) { syscall3(SYS_SHUTDOWN, 0, 0, 0); }

int socket(int type) { return (int)syscall3(SYS_SOCKET, type, 0, 0); }
int bind(int fd, unsigned short port) { return (int)syscall3(SYS_BIND, fd, port, 0); }
int sendto(int fd, const void *buf, int len, unsigned int ip, unsigned short port)
{ return (int)syscall5(SYS_SENDTO, fd, (long)buf, len, ip, port); }
int recvfrom(int fd, void *buf, int len, unsigned int *ip, unsigned short *port)
{ return (int)syscall5(SYS_RECVFROM, fd, (long)buf, len, (long)ip, (long)port); }
int connect(int fd, unsigned int ip, unsigned short port)
{ return (int)syscall3(SYS_CONNECT, fd, ip, port); }
int listen(int fd, int backlog) { return (int)syscall3(SYS_LISTEN, fd, backlog, 0); }
int accept(int fd) { return (int)syscall3(SYS_ACCEPT, fd, 0, 0); }
int sock_shutdown(int fd, int how) { return (int)syscall3(SYS_SOCKSHUT, fd, how, 0); }
int poll(struct pollfd *fds, int nfds, int timeout_ms)
{ return (int)syscall3(SYS_POLL, (long)fds, nfds, timeout_ms); }
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
int  input_read(struct input_event *ev) { return (int)syscall3(SYS_INPUT_READ, (long)ev, 0, 0); }
int  input_poll(struct input_event *ev) { return (int)syscall3(SYS_INPUT_READ, (long)ev, 1, 0); }
int  gfx_acquire(struct gfx_info *gi) { return (int)syscall3(SYS_GFX_ACQUIRE, (long)gi, 0, 0); }
int  gfx_flush(int x, int y, int w, int h) { return (int)syscall5(SYS_GFX_FLUSH, x, y, w, h, 0); }
int  seat_switch(int n) { return (int)syscall3(SYS_SEAT_SWITCH, n, 0, 0); }
int  openpty(int fd[2]) { return (int)syscall3(SYS_OPENPT, (long)fd, 0, 0); }
int  sys_getc(void) { char c; long n = syscall3(SYS_READ, 0, (long)&c, 1); return n == 1 ? (int)(unsigned char)c : -1; }
