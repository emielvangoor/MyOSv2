// syscall.c -- the kernel side of the system-call interface.
// =========================================================
//
// A user (EL0) thread puts a syscall number in x8 and arguments in x0.. and
// executes `svc #0`. That traps to EL1; el0_sync_handler saves the registers as
// a trap frame and calls do_syscall(), which performs the requested service and
// writes the return value back into x0 (so `eret` hands it to the user).

#include <stdint.h>
#include "syscall.h"
#include "errno.h"
#include "uart.h"
#include "sched.h"
#include "console.h"
#include "kprintf.h"
#include "vfs.h"
#include "proc.h"
#include "shm.h"
#include "pipe.h"
#include "kheap.h"
#include "signal.h"
#include "net.h"
#include "power.h"
#include "socket.h"
#include "poll.h"
#include "timer.h"
#include "input.h"
#include "gfx.h"
#include "vm.h"
#include "seat.h"

// Fill a Linux/aarch64 `struct stat` (128 bytes) for a vnode -- enough fields
// (mode, nlink, size, blksize/blocks) for musl's stat()/fstat() to be useful.
static void fill_stat(void *ubuf, struct vnode *vn)
{
    uint8_t *s = (uint8_t *)ubuf;
    for (int i = 0; i < 128; i++) { s[i] = 0; }
    uint32_t mode = (vn->type == VN_DIR) ? (040000 | 0755) : (0100000 | 0644);
    *(uint32_t *)(s + 16) = mode;                       // st_mode
    *(uint32_t *)(s + 20) = 1;                          // st_nlink
    *(int64_t  *)(s + 48) = (int64_t)vn->size;          // st_size
    *(int32_t  *)(s + 56) = 512;                        // st_blksize
    *(int64_t  *)(s + 64) = (int64_t)((vn->size + 511) / 512);  // st_blocks
}

// Resolve a user PATH against the current process's cwd into OUT as a canonical
// ABSOLUTE path: a relative path is joined onto cwd, then "." components are
// dropped and ".." pops the previous component (so "a/../b" -> "/b"). This is
// what gives MyOSv2 a real working directory -- openat/newfstatat run every
// path through it, so `tcc hello.c -o hello` writes hello in the cwd, not only
// at the root. OUT must hold at least 256 bytes.
static void resolve_path(const char *path, char *out, int outsz)
{
    char tmp[512];
    int n = 0;
    if (path[0] == '/') {                          // absolute: ignore cwd
        for (int i = 0; path[i] && n < (int)sizeof(tmp) - 1; i++) { tmp[n++] = path[i]; }
    } else {                                       // relative: cwd + "/" + path
        struct thread *t = sched_current();
        const char *c = t ? t->cwd : "/";
        for (int i = 0; c[i] && n < (int)sizeof(tmp) - 1; i++) { tmp[n++] = c[i]; }
        if (n == 0 || tmp[n - 1] != '/') { if (n < (int)sizeof(tmp) - 1) { tmp[n++] = '/'; } }
        for (int i = 0; path[i] && n < (int)sizeof(tmp) - 1; i++) { tmp[n++] = path[i]; }
    }
    tmp[n] = 0;

    // Canonicalize component by component into OUT.
    int olen = 0;
    int p = 0;
    while (tmp[p]) {
        while (tmp[p] == '/') { p++; }             // skip run of slashes
        if (!tmp[p]) { break; }
        int q = p;
        while (tmp[q] && tmp[q] != '/') { q++; }   // component is [p, q)
        int clen = q - p;
        if (clen == 1 && tmp[p] == '.') {
            // "." -> stay put
        } else if (clen == 2 && tmp[p] == '.' && tmp[p + 1] == '.') {
            while (olen > 0 && out[olen - 1] != '/') { olen--; }  // pop component
            if (olen > 0) { olen--; }                              // drop its '/'
        } else {
            if (olen < outsz - 1) { out[olen++] = '/'; }
            for (int k = p; k < q && olen < outsz - 1; k++) { out[olen++] = tmp[k]; }
        }
        p = q;
    }
    if (olen == 0) { out[olen++] = '/'; }          // everything collapsed -> root
    out[olen] = 0;
}

long do_syscall(struct trapframe *tf)
{
    uint64_t num = tf->x[8];
    long ret = -1;

    switch (num) {
    case SYS_WRITE: {                       // x0 = fd, x1 = ptr, x2 = len
        uint64_t fd = tf->x[0];
        const char *s = (const char *)(uintptr_t)tf->x[1];
        uint64_t len = tf->x[2];
        struct file **fds = sched_current_fds();
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {   // a stream socket -> TCP
            ret = socket_write(fds[fd]->sock, s, (int)len);
        } else if (fds && fd < 16 && fds[fd]) {    // an open file/pipe takes precedence
            ret = vfs_write(fds[fd], s, len);
        } else if (fd == 1 || fd == 2) {    // bare stdout/stderr -> UART console
            for (uint64_t i = 0; i < len; i++) { uart_putc(s[i]); }
            ret = (long)len;
        } else { ret = -EBADF; }
        break;
    }
    case SYS_OPENAT: {                      // x0=dirfd, x1=path, x2=flags, x3=mode
        // dirfd is AT_FDCWD only; paths resolve against the process cwd (a
        // relative path is joined onto it -- see resolve_path). O_CREAT makes a
        // missing file; O_TRUNC is not yet honored (the SFS can't shrink).
        char path[256];
        resolve_path((const char *)(uintptr_t)tf->x[1], path, sizeof(path));
        int flags = (int)tf->x[2];
        struct file **fds = sched_current_fds();
        if (!fds) { ret = -EBADF; break; }
        struct file *f = vfs_open(path);
        if (!f && (flags & O_CREAT)) {      // create it, then open it
            vfs_create(path, VN_FILE);
            f = vfs_open(path);
        }
        if (!f) { ret = -ENOENT; break; }
        ret = -EMFILE;
        for (int i = 3; i < 16; i++) {
            if (!fds[i]) { fds[i] = f; ret = i; break; }
        }
        if (ret < 0) { vfs_close(f); }
        break;
    }
    case SYS_READ: {                        // x0 = fd, x1 = buf, x2 = len
        uint64_t fd = tf->x[0];
        void *buf = (void *)(uintptr_t)tf->x[1];
        uint64_t len = tf->x[2];
        struct file **fds = sched_current_fds();
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {   // a stream socket -> TCP
            ret = socket_read(fds[fd]->sock, buf, (int)len);
        } else if (fds && fd < 16 && fds[fd]) {     // an open file/pipe takes precedence
            ret = vfs_read(fds[fd], buf, len);
        } else if (fd == 0) {                // bare stdin: one char from the keyboard
            char *cb = (char *)buf;
            if (len == 0) { ret = 0; break; }
            int ch = console_getc();         // BLOCKS (sleeps) until the UART IRQ
            if (ch < 0) { ret = 0; break; }  // interrupted by a signal (EINTR)
            cb[0] = (char)ch;
            ret = 1;
        } else { ret = -EBADF; }
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
        else { ret = -EBADF; }
        break;
    }
    case SYS_GETPID:
        ret = sched_current_id();
        break;
    case SYS_SET_TID_ADDRESS:                // x0 = clear-on-exit ptr (ignored)
        ret = sched_current_id();            // single-threaded: just our TID
        break;
    case SYS_IOCTL:                          // x0=fd, x1=request, x2=arg
        ret = -ENOTTY;                       // no real ttys -> musl uses buffered I/O
        break;
    case SYS_WRITEV: {                       // x0=fd, x1=struct iovec*, x2=iovcnt
        uint64_t fd = tf->x[0];
        const uint64_t *iov = (const uint64_t *)(uintptr_t)tf->x[1];  // {base,len} pairs
        int n = (int)tf->x[2];
        struct file **fds = sched_current_fds();
        long total = 0;
        for (int i = 0; i < n; i++) {
            const char *base = (const char *)(uintptr_t)iov[i * 2];
            uint64_t len = iov[i * 2 + 1];
            long w;
            if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
                w = socket_write(fds[fd]->sock, base, (int)len);
            } else if (fds && fd < 16 && fds[fd]) {
                w = vfs_write(fds[fd], base, len);
            } else if (fd == 1 || fd == 2) {
                for (uint64_t j = 0; j < len; j++) { uart_putc(base[j]); }
                w = (long)len;
            } else { ret = total ? total : -EBADF; goto writev_done; }
            if (w < 0) { if (!total) { total = w; } break; }
            total += w;
        }
        ret = total;
    writev_done:
        break;
    }
    case SYS_YIELD:
        yield();
        ret = 0;
        break;
    case SYS_SLEEP:                          // x0 = milliseconds
        sleep_ms(tf->x[0]);
        ret = 0;
        break;
    case SYS_EXIT:                           // single thread; we're single-threaded
    case SYS_EXIT_GROUP:                      // musl's exit() -> exit_group
        thread_exit((int)tf->x[0]);          // x0 = exit status; does not return
        ret = 0;
        break;
    case SYS_CLONE:                          // x0=flags, x1=child_stack, ...
        // The fork-equivalent only (flags=SIGCHLD, no CLONE_VM): musl's fork()
        // and our own fork() both arrive here. Thread clones (CLONE_VM, a child
        // stack) are out of scope until a program needs them.
        if (tf->x[0] & 0x00000100UL /* CLONE_VM */) { ret = -ENOSYS; break; }
        ret = sched_fork(tf);                // child pid (parent); child gets 0
        break;
    case SYS_EXECVE:                         // x0=path, x1=argv, x2=envp (envp ignored)
        ret = proc_exec(tf, (const char *)(uintptr_t)tf->x[0],
                            (char *const *)(uintptr_t)tf->x[1]);
        break;                               // on success tf is rewritten to the new image
    case SYS_WAIT4: {                        // x0=pid, x1=int* status, x2=options, x3=rusage
        int raw = 0;
        long pid = sched_wait(&raw);         // reap any child (pid ignored); raw code
        if (pid >= 0 && tf->x[1]) {          // encode Linux-style: WIFEXITED, code<<8
            *(int *)(uintptr_t)tf->x[1] = (raw & 0xff) << 8;
        }
        ret = pid;
        break;
    }
    case SYS_RT_SIGPROCMASK:                 // x0=how, x1=set, x2=oldset, x3=sigsetsize
        ret = 0;                             // no per-process signal mask yet -> no-op
        break;
    case SYS_GETUID: case SYS_GETEUID:       // single-user system: everyone is root
    case SYS_GETGID: case SYS_GETEGID:
        ret = 0;
        break;
    case SYS_UNAME: {                        // x0 = struct utsname* (6 x 65-byte fields)
        char *u = (char *)(uintptr_t)tf->x[0];
        for (int i = 0; i < 6 * 65; i++) { u[i] = 0; }
        const char *f[6] = { "Linux", "myosv2", "6.0.0-myosv2",
                             "#1 MyOSv2", "aarch64", "(none)" };
        for (int k = 0; k < 6; k++) {
            const char *s = f[k]; char *d = u + k * 65;
            for (int j = 0; s[j] && j < 64; j++) { d[j] = s[j]; }
        }
        ret = 0;
        break;
    }
    case SYS_LSEEK: {                        // x0=fd, x1=offset, x2=whence
        uint64_t fd = tf->x[0];
        long off = (long)tf->x[1];
        int whence = (int)tf->x[2];          // 0=SET, 1=CUR, 2=END
        struct file **fds = sched_current_fds();
        if (!fds || fd >= 16 || !fds[fd] || !fds[fd]->vnode) { ret = -EBADF; break; }
        long base = (whence == 1) ? (long)fds[fd]->off
                  : (whence == 2) ? (long)fds[fd]->vnode->size : 0;
        long neu = base + off;
        if (neu < 0) { ret = -EINVAL; break; }
        fds[fd]->off = (uint64_t)neu;
        ret = neu;
        break;
    }
    case SYS_FSTAT: {                        // x0=fd, x1=statbuf
        uint64_t fd = tf->x[0];
        struct file **fds = sched_current_fds();
        if (!fds || fd >= 16 || !fds[fd] || !fds[fd]->vnode) { ret = -EBADF; break; }
        fill_stat((void *)(uintptr_t)tf->x[1], fds[fd]->vnode);
        ret = 0;
        break;
    }
    case SYS_NEWFSTATAT: {                    // x0=dirfd, x1=path, x2=statbuf, x3=flags
        char path[256];
        resolve_path((const char *)(uintptr_t)tf->x[1], path, sizeof(path));
        struct vnode *vn = vfs_lookup(path);
        if (!vn) { ret = -ENOENT; break; }
        fill_stat((void *)(uintptr_t)tf->x[2], vn);
        ret = 0;
        break;
    }
    case SYS_GETDENTS64: {                   // x0=fd, x1=buf, x2=count -> bytes / 0 (end)
        // Linux reads a directory by repeatedly calling getdents64 on an open
        // dirfd: each call packs as many variable-length `struct linux_dirent64`
        // records as fit into the user buffer and returns the byte count; a
        // return of 0 means end-of-directory. The kernel must remember where it
        // left off -- we reuse the open file's `off` as the readdir index (it's
        // meaningless as a byte offset on a directory anyway). busybox `ls`
        // drives this. The on-disk record layout (packed, then 8-byte aligned so
        // each next record's u64 fields stay aligned):
        //   d_ino   u64 @0   d_off  s64 @8   d_reclen u16 @16
        //   d_type  u8  @18   d_name[] @19 (NUL-terminated)
        uint64_t fd = tf->x[0];
        uint8_t *ubuf = (uint8_t *)(uintptr_t)tf->x[1];
        uint64_t cap = tf->x[2];
        struct file **fds = sched_current_fds();
        if (!fds || fd >= 16 || !fds[fd] || !fds[fd]->vnode) { ret = -EBADF; break; }
        struct vnode *dir = fds[fd]->vnode;
        if (dir->type != VN_DIR) { ret = -ENOTDIR; break; }

        uint64_t used = 0;
        char name[256];
        while (vfs_readdir(dir, (int)fds[fd]->off, name) == 0) {
            // Length of this name, then the 8-byte-aligned record size.
            uint64_t nlen = 0;
            while (name[nlen] && nlen < 255) { nlen++; }
            uint64_t reclen = (19 + nlen + 1 + 7) & ~(uint64_t)7;
            if (used + reclen > cap) { break; }   // buffer full: stop, keep `off`

            // Resolve the child's type for d_type so `ls` can colorize/recurse
            // without a stat() per entry (DT_DIR=4, DT_REG=8, DT_UNKNOWN=0).
            uint8_t dtype = 0;
            struct vnode *child = dir->ops->lookup ? dir->ops->lookup(dir, name) : 0;
            if (child) { dtype = (child->type == VN_DIR) ? 4 : 8; }

            uint8_t *rec = ubuf + used;
            *(uint64_t *)(rec + 0)  = (uint64_t)(fds[fd]->off + 1);  // d_ino (synthetic, nonzero)
            *(int64_t  *)(rec + 8)  = (int64_t)(fds[fd]->off + 1);   // d_off (next index)
            *(uint16_t *)(rec + 16) = (uint16_t)reclen;              // d_reclen
            *(uint8_t  *)(rec + 18) = dtype;                         // d_type
            for (uint64_t i = 0; i < nlen; i++) { rec[19 + i] = (uint8_t)name[i]; }
            rec[19 + nlen] = 0;                                      // NUL terminator
            // Zero the alignment padding so we never leak kernel bytes.
            for (uint64_t i = 19 + nlen + 1; i < reclen; i++) { rec[i] = 0; }

            used += reclen;
            fds[fd]->off++;
        }
        ret = (long)used;   // 0 once readdir is exhausted -> end-of-directory
        break;
    }
    case SYS_CHDIR: {                        // x0 = path -> 0 / -errno
        // Resolve PATH against the current cwd, require it to be a directory,
        // then record it as the process's new cwd (canonical absolute form).
        char path[256];
        resolve_path((const char *)(uintptr_t)tf->x[0], path, sizeof(path));
        struct vnode *vn = vfs_lookup(path);
        if (!vn) { ret = -ENOENT; break; }
        if (vn->type != VN_DIR) { ret = -ENOTDIR; break; }
        struct thread *t = sched_current();
        if (!t) { ret = -EINVAL; break; }
        int i = 0;
        while (path[i] && i < (int)sizeof(t->cwd) - 1) { t->cwd[i] = path[i]; i++; }
        t->cwd[i] = 0;
        ret = 0;
        break;
    }
    case SYS_GETCWD: {                       // x0 = buf, x1 = size -> len incl NUL / -errno
        // Linux getcwd writes the NUL-terminated cwd and returns its length
        // including the terminator; musl's wrapper hands back the buffer.
        char *ubuf = (char *)(uintptr_t)tf->x[0];
        uint64_t size = tf->x[1];
        struct thread *t = sched_current();
        const char *c = t ? t->cwd : "/";
        uint64_t len = 0;
        while (c[len]) { len++; }
        if (size < len + 1) { ret = -ERANGE; break; }   // caller's buffer too small
        for (uint64_t i = 0; i < len; i++) { ubuf[i] = c[i]; }
        ubuf[len] = 0;
        ret = (long)(len + 1);
        break;
    }
    case SYS_BRK: {                          // x0 = new break (0 = query)
        // Linux brk: set the break to x0 and return the resulting break; a 0
        // (or out-of-range) request just returns the current one. as_sbrk is
        // increment-based, so we translate. musl uses this for its main heap.
        struct addrspace *as = sched_current_as();
        uint64_t cur = as_sbrk(as, 0);       // current break (incr 0 = no change)
        uint64_t want = tf->x[0];
        if (want < cur) { ret = (long)cur; break; }     // query / shrink-ignored
        uint64_t old = as_sbrk(as, (long)(want - cur));
        ret = (old == (uint64_t)-1) ? (long)cur : (long)want;
        break;
    }
    case SYS_MMAP: {                         // x0=addr x1=len x2=prot x3=flags ...
        // Anonymous mappings only (what musl's malloc uses); file-backed mmap
        // is out of scope until a program needs it. Returns the base VA, or
        // -ENOMEM (which musl turns into MAP_FAILED).
        uint64_t va = as_mmap(sched_current_as(), tf->x[1]);
        ret = va ? (long)va : -ENOMEM;
        break;
    }
    case SYS_MUNMAP:                         // x0 = va, x1 = len
        ret = as_munmap(sched_current_as(), tf->x[0], tf->x[1]);
        break;
    case SYS_SHM_CREATE:                     // x0 = len
        ret = shm_create(tf->x[0]);
        break;
    case SYS_SHM_MAP:                        // x0 = handle
        ret = (long)shm_map(sched_current_as(), (int)tf->x[0]);
        break;
    case SYS_PIPE: {                         // x0 = int fd[2]
        int *ufd = (int *)(uintptr_t)tf->x[0];
        struct file **fds = sched_current_fds();
        ret = -1;
        if (!fds) { break; }
        struct pipe *p = pipe_alloc();
        struct file *rf = kmalloc(sizeof(struct file));
        struct file *wf = kmalloc(sizeof(struct file));
        // EVERY field must be initialized: kmalloc doesn't zero, and the read/
        // write/close paths dispatch on ->sock before ->pipe -- a recycled
        // block that used to be a socket file would otherwise hand this pipe
        // a stale socket pointer (found live: pipelines returned 0 bytes after
        // any socket-using program had exited).
        rf->vnode = 0; rf->off = 0; rf->pipe = p; rf->sock = 0; rf->writable = 0; rf->ref = 1;
        wf->vnode = 0; wf->off = 0; wf->pipe = p; wf->sock = 0; wf->writable = 1; wf->ref = 1;
        // Allocate from fd 3 up: fds 0/1/2 stay reserved for stdin/stdout/stderr
        // (NULL there means "the console"), so a pipe never clobbers them.
        int r = -1, w = -1;
        for (int i = 3; i < 16 && r < 0; i++) { if (!fds[i]) { fds[i] = rf; r = i; } }
        for (int i = 3; i < 16 && w < 0; i++) { if (!fds[i]) { fds[i] = wf; w = i; } }
        if (r < 0 || w < 0) {                // no room: undo
            if (r >= 0) { fds[r] = 0; } vfs_close(rf); vfs_close(wf);
            break;
        }
        ufd[0] = r; ufd[1] = w; ret = 0;
        break;
    }
    case SYS_DUP2: {                         // x0 = oldfd, x1 = newfd
        struct file **fds = sched_current_fds();
        uint64_t o = tf->x[0], n = tf->x[1];
        if (!fds || o >= 16 || n >= 16 || !fds[o]) { ret = -1; break; }
        if (o != n) {
            if (fds[n]) { vfs_close(fds[n]); }
            fds[n] = file_dup(fds[o]);
        }
        ret = (long)n;
        break;
    }
    case SYS_KILL:                           // x0 = pid, x1 = sig
        ret = sched_kill((int)tf->x[0], (int)tf->x[1]);
        break;
    case SYS_SIGNAL: {                        // x0 = sig, x1 = handler, x2 = trampoline
        struct thread *t = sched_current();
        int sig = (int)tf->x[0];
        if (t && sig > 0 && sig < 32) {
            t->sig_handler[sig] = (uint64_t (*)(int))(uintptr_t)tf->x[1];
            t->sig_tramp = tf->x[2];
            ret = 0;
        } else { ret = -1; }
        break;
    }
    case SYS_PING:                            // x0 = ip (host order), x1 = int* ms
        ret = net_ping((uint32_t)tf->x[0], (int *)(uintptr_t)tf->x[1]);
        break;
    case SYS_RESOLVE: {                       // x0 = hostname -> IP (0 = failure)
        uint32_t ip = 0;
        if (net_resolve((const char *)(uintptr_t)tf->x[0], &ip) != 0) { ip = 0; }
        ret = (long)ip;
        break;
    }
    case SYS_SHUTDOWN:                        // halt the machine (does not return)
        power_off();
        break;
    case SYS_SOCKET: {                        // x0 = type -> fd
        struct file **fds = sched_current_fds();
        struct socket *s = socket_alloc((int)tf->x[0]);
        if (!fds || !s) { if (s) { socket_free(s); } ret = -1; break; }
        struct file *f = kmalloc(sizeof(struct file));
        f->vnode = 0; f->off = 0; f->pipe = 0; f->sock = s; f->writable = 0; f->ref = 1;
        ret = -1;
        for (int i = 3; i < 16; i++) { if (!fds[i]) { fds[i] = f; ret = i; break; } }
        if (ret < 0) { socket_free(s); kfree(f); }   // table full
        break;
    }
    case SYS_BIND: {                          // x0 = fd, x1 = port
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            ret = socket_bind(fds[fd]->sock, (uint16_t)tf->x[1]);
        } else { ret = -1; }
        break;
    }
    case SYS_SENDTO: {                        // x0=fd x1=buf x2=len x3=ip x4=port
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            ret = socket_sendto(fds[fd]->sock, (const void *)(uintptr_t)tf->x[1],
                                (int)tf->x[2], (uint32_t)tf->x[3], (uint16_t)tf->x[4]);
        } else { ret = -1; }
        break;
    }
    case SYS_RECVFROM: {                      // x0=fd x1=buf x2=len x3=uint*ip x4=uint16*port
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            ret = socket_recvfrom(fds[fd]->sock, (void *)(uintptr_t)tf->x[1], (int)tf->x[2],
                                  (uint32_t *)(uintptr_t)tf->x[3], (uint16_t *)(uintptr_t)tf->x[4]);
        } else { ret = -1; }
        break;
    }
    case SYS_CONNECT: {                       // x0 = fd, x1 = ip, x2 = port
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            ret = socket_connect(fds[fd]->sock, (uint32_t)tf->x[1], (uint16_t)tf->x[2]);
        } else { ret = -1; }
        break;
    }
    case SYS_LISTEN: {                        // x0 = fd, x1 = backlog
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            ret = socket_listen(fds[fd]->sock, (int)tf->x[1]);
        } else { ret = -1; }
        break;
    }
    case SYS_ACCEPT: {                        // x0 = fd -> new connected fd
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        ret = -1;
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            struct socket *ns = socket_accept(fds[fd]->sock);   // blocks for a peer
            if (ns) {
                struct file *f = kmalloc(sizeof(struct file));
                f->vnode = 0; f->off = 0; f->pipe = 0; f->sock = ns; f->writable = 0; f->ref = 1;
                for (int i = 3; i < 16; i++) { if (!fds[i]) { fds[i] = f; ret = i; break; } }
                if (ret < 0) { socket_free(ns); kfree(f); }     // fd table full
            }
        }
        break;
    }
    case SYS_POLL: {                          // x0=pollfd*, x1=nfds, x2=timeout_ms
        struct file **fds = sched_current_fds();
        struct pollfd *pf = (struct pollfd *)(uintptr_t)tf->x[0];
        int nfds = (int)tf->x[1];
        int timeout = (int)tf->x[2];
        if (!fds || !pf || nfds < 0 || nfds > 16) { ret = -1; break; }
        uint64_t start = timer_now_us();
        for (;;) {
            int r = poll_scan(fds, pf, nfds, 16);
            if (r > 0) { ret = r; break; }            // at least one fd is ready
            if (timeout == 0) { ret = 0; break; }     // non-blocking poll
            struct thread *t = sched_current();
            if (t && t->sig_pending) { ret = -1; break; }   // EINTR
            if (timeout > 0 &&
                (timer_now_us() - start) >= (uint64_t)timeout * 1000) { ret = 0; break; }
            net_pump();                               // let socket readiness advance
            net_wait(20);                             // sleep on the NIC IRQ / timer
        }
        break;
    }
    case SYS_SOCKSHUT: {                      // x0=fd, x1=how
        struct file **fds = sched_current_fds();
        uint64_t fd = tf->x[0];
        if (fds && fd < 16 && fds[fd] && fds[fd]->sock) {
            ret = socket_shutdown(fds[fd]->sock, (int)tf->x[1]);
        } else { ret = -1; }
        break;
    }
    case SYS_INPUT_READ: {                    // x0 = struct input_event*  -> 0 / -1
        struct input_event *out = (struct input_event *)(uintptr_t)tf->x[0];
        if (!out) { ret = -1; break; }
        // Block (sleep/wake on the input waitq, never spin) until a device
        // delivers an event. A pending signal aborts the wait (EINTR), so
        // Ctrl-C can stop a reader. In the pre-IRQ test environment the wait
        // would never be woken, so poll once more and give up instead.
        // Seat routing: when display clients exist, only the ACTIVE one may
        // consume events -- inactive VMs sleep here until switched in.
        // Ctrl-Alt-F1..F4 is the kernel hotkey: it is consumed HERE (by the
        // active reader's drain) and never reaches any client.
        static int hk_ctrl, hk_alt;
        for (;;) {
            int apid = seat_active_pid();
            if (apid != -1 && (int)sched_current_id() != apid) {
                struct thread *t0 = sched_current();
                if (t0 && t0->sig_pending) { ret = -1; break; }
                if (!sched_irqs_live()) { ret = -1; break; }
                sched_wait_event(input_waitq(), 100);
                continue;
            }
            if (input_poll_event(out)) {
                if (out->type == EV_KEY && out->code == 29) { hk_ctrl = (out->value != 0); }
                if (out->type == EV_KEY && out->code == 56) { hk_alt  = (out->value != 0); }
                if (out->type == EV_KEY && out->value == 1 && hk_ctrl && hk_alt &&
                    out->code >= 59 && out->code <= 62) {     // F1..F4
                    int n = (int)out->code - 58;
                    if (seat_switch(n) == 0) {
                        gfx_show(n);                          // replay its pixels
                        sched_wake(input_waitq());            // unblock the new owner
                    }
                    continue;                                 // consumed
                }
                ret = 0; break;
            }
            if (tf->x[1] == 1) { ret = -1; break; }         // non-blocking probe
            struct thread *t = sched_current();
            if (t && t->sig_pending) { ret = -1; break; }   // EINTR
            if (!sched_irqs_live()) { ret = -1; break; }    // KTEST: don't sleep forever
            sched_wait_event(input_waitq(), 100);
        }
        break;
    }
    case SYS_SETPGID:                        // x0 = pid (0=self), x1 = pgid (0=own)
        ret = sched_setpgid((int)tf->x[0], (int)tf->x[1]);
        break;
    case SYS_SEAT_SWITCH: {                   // x0 = seat number (1-based)
        int n = (int)tf->x[0];
        if (seat_switch(n) != 0) { ret = -1; break; }
        gfx_show(n);
        sched_wake(input_waitq());
        ret = 0;
        break;
    }
    case SYS_GFX_ACQUIRE: {                   // x0 = struct {void* fb; u32 w,h,pitch}*
        struct { uint64_t fb; uint32_t w, h, pitch; } *gi =
            (void *)(uintptr_t)tf->x[0];
        struct addrspace *as = sched_current_as();
        if (!gi || !as) { ret = -1; break; }
        // Every client gets its OWN framebuffer + gpu resource = one SEAT
        // (the VT model). Re-acquiring returns the seat you already own.
        int pid = (int)sched_current_id();
        uint64_t pa = seat_fb(seat_register(pid, 0));
        if (!pa) {
            pa = gfx_fb_new();
            if (!pa) { ret = -1; break; }
            int seat = seat_register(pid, pa);
            if (seat < 0) { ret = -1; break; }
            if (gfx_resource_setup(seat, pa) != 0) { ret = -1; break; }
            if (seat == seat_active()) { gfx_show(seat); }  // first client
        }
        // Map the kernel framebuffer's pages into THIS process. The pages are
        // contiguous, so the page array is just pa, pa+4K, ...
        uint64_t npages = ((uint64_t)GFX_W * GFX_H * 4 + 4095) / 4096;
        static uint64_t pages[((uint64_t)GFX_W * GFX_H * 4 + 4095) / 4096];
        for (uint64_t i = 0; i < npages; i++) { pages[i] = pa + i * 4096; }
        uint64_t va = as_map_phys(as, pages, npages);
        if (!va) { ret = -1; break; }
        gi->fb = va; gi->w = GFX_W; gi->h = GFX_H; gi->pitch = GFX_W * 4;
        ret = 0;
        break;
    }
    case SYS_GFX_FLUSH: {                     // x0=x, x1=y, x2=w, x3=h
        // Only the ACTIVE seat's flushes reach the device; an inactive VM
        // keeps rendering into its own framebuffer and is simply not shown
        // (its full content reappears via gfx_show on switch-in).
        if ((int)sched_current_id() != seat_active_pid()) { ret = 0; break; }
        uint64_t x = tf->x[0], y = tf->x[1], w = tf->x[2], h = tf->x[3];
        if (x >= GFX_W || y >= GFX_H) { ret = -1; break; }
        if (x + w > GFX_W) { w = GFX_W - x; }   // clamp: a bad rect must not
        if (y + h > GFX_H) { h = GFX_H - y; }   // reach the device
        ret = gfx_flush_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
        break;
    }
    case SYS_SIGRETURN: {                     // restore the pre-signal trap frame
        const uint64_t *saved = (const uint64_t *)(uintptr_t)tf->sp_el0;
        uint64_t *d = (uint64_t *)tf;
        for (unsigned i = 0; i < sizeof(struct trapframe) / 8; i++) { d[i] = saved[i]; }
        ret = (long)tf->x[0];                 // keep the restored x0 (don't clobber below)
        break;
    }
    case SYS_REPORT:                         // x0 = pid, x1 = value read back
        kprintf("  [user] process %d read %d  (%s)\n",
                (int)tf->x[0], (int)tf->x[1],
                (long)tf->x[0] == (long)tf->x[1] ? "ISOLATED" : "LEAKED");
        ret = 0;
        break;
    default:
        // Unknown syscall: report it (so a musl binary tells us what it needs)
        // and return -ENOSYS, the Linux convention. The old -1 was ambiguous.
        kprintf("  [syscall] unhandled #%lu (x0=%lx x1=%lx x2=%lx)\n",
                (unsigned long)num, (unsigned long)tf->x[0],
                (unsigned long)tf->x[1], (unsigned long)tf->x[2]);
        ret = -ENOSYS;
        break;
    }

    tf->x[0] = (uint64_t)ret;                // return value goes in x0
    return ret;
}
