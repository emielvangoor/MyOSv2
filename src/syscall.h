// syscall.h -- system call numbers and the kernel-side dispatcher.
#pragma once
#include "exceptions.h"   // struct trapframe

// openat() flags + special dirfd we honor (Linux/aarch64 values).
#define AT_FDCWD   (-100)
#define O_WRONLY   01
#define O_RDWR     02
#define O_CREAT    0100
#define O_TRUNC    01000
#define O_APPEND   02000   // `>>`: writes go to end-of-file (open at size, not 0)

// Syscall numbers (the user passes one in x8). Phase 28 migrates this ABI to
// Linux/aarch64 so unmodified musl binaries run. Step 1 (here): carve the
// MyOSv2-ONLY syscalls -- the ones with no Linux equivalent -- into a private
// high range (MYOS_SYS_BASE+) so they never collide with Linux numbers, which
// frees the low numbers (incl. 17/29/... that Linux uses) for the POSIX
// migration that follows. The POSIX-ish numbers below are still MyOSv2's old
// values for now; later steps give them their Linux numbers + semantics.
#define MYOS_SYS_BASE 0x1000      // MyOSv2-private syscalls live at 0x1000+

// --- migrated to Linux/aarch64 numbers + negative-errno (step 2, "clean" set,
//     identical arg registers to Linux) ---
#define SYS_READ   63  // x0=fd, x1=buf, x2=len -> bytes read / -errno
#define SYS_WRITE  64  // x0=fd, x1=ptr, x2=len -> bytes written / -errno
#define SYS_CLOSE  57  // x0=fd -> 0 / -errno
#define SYS_EXIT   93  // x0=status -> (does not return)
#define SYS_EXIT_GROUP 94 // x0=status -> (does not return; musl's _Exit/exit)
#define SYS_GETPID 172 //                 -> current thread id
#define SYS_YIELD  124 // sched_yield     -> 0

#define SYS_OPENAT 56  // x0=dirfd(AT_FDCWD), x1=path, x2=flags, x3=mode -> fd / -errno
#define SYS_IOCTL  29  // x0=fd, x1=req, x2=arg -> 0 for tty reqs (TCGETS/...), else -ENOTTY
#define SYS_WRITEV 66  // x0=fd, x1=struct iovec*, x2=iovcnt -> bytes / -errno
#define SYS_SET_TID_ADDRESS 96  // x0=ptr (ignored; single-threaded) -> tid
#define SYS_RT_SIGACTION   134  // real aarch64 sigaction busybox/musl emits
#define SYS_RT_SIGPROCMASK 135  // no per-process signal mask yet -> 0 (no-op)
#define SYS_RT_SIGRETURN   139  // real aarch64 sigreturn (mirrors SYS_SIGRETURN=22)
#define SYS_GETCWD 17  // x0=buf, x1=size -> bytes (incl NUL) written / -errno
#define SYS_CHDIR  49  // x0=path -> 0 / -errno (sets the process cwd)
#define SYS_GETDENTS64 61 // x0=fd, x1=buf, x2=count -> bytes of dirents / 0 (end)
#define SYS_LSEEK  62  // x0=fd, x1=offset, x2=whence -> new offset / -errno
#define SYS_UNAME  160 // x0=struct utsname* -> 0 (reports Linux/aarch64)
#define SYS_GETUID  174 // -> 0 (single-user: root)
#define SYS_GETEUID 175 // -> 0
#define SYS_GETGID  176 // -> 0
#define SYS_GETEGID 177 // -> 0
#define SYS_NEWFSTATAT 79 // x0=dirfd, x1=path, x2=statbuf, x3=flags -> 0 / -errno
#define SYS_FSTAT  80  // x0=fd, x1=statbuf -> 0 / -errno
#define SYS_BRK    214 // x0=new break (0=query) -> resulting break
#define SYS_MUNMAP 215 // x0=va, x1=len -> 0 / -errno
#define SYS_MMAP   222 // x0=addr,x1=len,... (anonymous) -> base VA / -ENOMEM

#define SYS_CLONE  220 // x0=flags(SIGCHLD), x1=child_stack -> child pid / 0 in child
#define SYS_EXECVE 221 // x0=path, x1=argv, x2=envp -> replaces image; -errno on fail
#define SYS_WAIT4  260 // x0=pid, x1=int* status, x2=options, x3=rusage -> pid / -errno

// --- real Linux/aarch64 numbers added for busybox + musl compatibility ---
// busybox and other musl-linked binaries use the numbers from
// <asm-generic/unistd.h>; our native MyOSv2 programs use lower custom numbers
// for the same operations. Both sets must coexist: the low numbers are what
// /bin/sh (native) and our own test helpers call; the real numbers are what
// busybox's libc emits. Both paths call the same kernel logic.
#define SYS_PPOLL          73   // x0=pollfd*, x1=nfds, x2=timespec* (NULL=block), x3=sigmask
#define SYS_FCNTL          25   // x0=fd, x1=cmd, x2=arg  (file-descriptor control)
#define SYS_DUP3           24   // x0=oldfd, x1=newfd, x2=flags -> newfd / -errno (musl's dup2())
#define SYS_PIPE2          59   // x0=int fd[2], x1=flags -> 0 / -errno (musl's pipe())
#define SYS_NANOSLEEP      101  // x0=const timespec* req, x1=timespec* rem -> 0 (musl's sleep())
#define SYS_UNLINKAT       35   // x0=dirfd, x1=path, x2=flags -> 0 / -errno (rm/rmdir)
#define SYS_FCHMODAT       53   // x0=dirfd, x1=path, x2=mode, x3=flags -> 0 / -errno (chmod)
#define SYS_UTIMENSAT      88   // x0=dirfd, x1=path, x2=times, x3=flags -> 0 / -errno (touch)
#define SYS_FACCESSAT      48   // x0=dirfd, x1=path, x2=mode, x3=flags -> 0 / -errno (test -e)
#define SYS_READLINKAT     78   // x0=dirfd, x1=path, x2=buf, x3=bufsiz -> len / -errno
#define SYS_FTRUNCATE      46   // x0=fd, x1=length -> 0 / -errno (length 0 only)
#define SYS_SENDFILE       71   // x0=out_fd, x1=in_fd, x2=off*, x3=count -> bytes / -errno
#define SYS_MKDIRAT        34   // x0=dirfd, x1=path, x2=mode -> 0 / -errno (mkdir)
#define SYS_SYMLINKAT      36   // x0=target, x1=newdirfd, x2=linkpath -> 0 / -errno (ln -s)
#define SYS_LINKAT         37   // x0=odirfd,x1=oldpath,x2=ndirfd,x3=newpath,x4=flags (ln)
#define SYS_RENAMEAT       38   // x0=odirfd, x1=oldpath, x2=ndirfd, x3=newpath -> 0 (mv)
#define SYS_RENAMEAT2      276  // x0..x3 as renameat, x4=flags -> 0 / -errno (mv)

// AT_* flags + faccessat/unlinkat flag values (Linux/aarch64).
#define AT_REMOVEDIR        0x200   // unlinkat: remove a directory instead of a file
#define AT_SYMLINK_NOFOLLOW 0x100   // *at: act on the link itself, not its target
#define SYS_CLOCK_GETTIME  113  // x0=clockid (ignored), x1=struct timespec*  -> 0
#define SYS_KILL_LINUX     129  // real aarch64 kill -- legacy SYS_KILL=20 kept for native
#define SYS_SETPGID_LINUX  154  // real aarch64 setpgid  -- legacy SYS_SETPGID=44 kept
#define SYS_GETPGID        155  // x0=pid (0=self) -> process group id
#define SYS_GETSID         156  // x0=pid (0=self) -> session id (~ pgid here)
#define SYS_SETSID         157  // -> new session id (our pid)
#define SYS_GETTIMEOFDAY   169  // x0=struct timeval*, x1=tz (ignored)  -> 0
#define SYS_GETPPID        173  // -> parent pid (or 1 if no parent)

// fcntl(2) command codes (asm-generic values, same as Linux).
// FD_CLOEXEC is not tracked yet -- F_GETFD/F_SETFD are accepted as no-ops,
// F_GETFL always returns O_RDWR (2) which is sufficient for busybox's probes.
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030  // ash saves redirected fds with this (dup + cloexec)

// --- still on old MyOSv2 numbers (migrate in later steps) ---
#define SYS_SLEEP  3   // x0=ms           -> 0
#define SYS_READDIR 10 // x0=path, x1=index, x2=namebuf -> 0 (name) / -1 (done)
#define SYS_PIPE   18  // x0=int fd[2] -> fills {readfd, writefd}; 0 / -1
#define SYS_DUP2   19  // x0=old, x1=new -> new (or -1)
#define SYS_KILL   20  // x0=pid, x1=sig -> 0 / -1
#define SYS_SIGNAL 21  // x0=sig, x1=handler, x2=trampoline -> 0 / -1
#define SYS_SIGRETURN 22 // restore the pre-signal context (used by the trampoline)
#define SYS_SETPGID     44 // x0=pid (0=self), x1=pgid (0=pid's own id) -> 0/-1

// --- MyOSv2-private syscalls (no Linux equivalent) -> high range ---
#define SYS_REPORT      (MYOS_SYS_BASE + 0)  // x0=pid, x1=value -> kernel prints
#define SYS_SHM_CREATE  (MYOS_SYS_BASE + 1)  // x0=len -> shm handle (or -1)
#define SYS_SHM_MAP     (MYOS_SYS_BASE + 2)  // x0=handle -> base VA (or -1)
#define SYS_PING        (MYOS_SYS_BASE + 3)  // x0=ip, x1=int* ms -> 0 / -1
#define SYS_RESOLVE     (MYOS_SYS_BASE + 4)  // x0=hostname -> IP (0 on failure)
#define SYS_SHUTDOWN    (MYOS_SYS_BASE + 5)  // halt the machine
#define SYS_INPUT_READ  (MYOS_SYS_BASE + 6)  // x0=struct input_event* -> 0 / -1
#define SYS_GFX_ACQUIRE (MYOS_SYS_BASE + 7)  // x0=struct gfx_info* -> 0 / -1
#define SYS_GFX_FLUSH   (MYOS_SYS_BASE + 8)  // x0=x,x1=y,x2=w,x3=h -> 0 / -1
#define SYS_SEAT_SWITCH (MYOS_SYS_BASE + 9)  // x0=seat (1-based) -> 0 / -1

// MyOSv2's socket API is a custom simplified interface (port-based, no
// sockaddr), used only by native programs (the Lisp networking primitives over
// ulib) -- musl binaries use the real Linux socket numbers (198+), which we do
// not implement. These USED to sit at 31-39, but those are real Linux numbers
// for the file-management *at family (mknodat/mkdirat/unlinkat/symlinkat/
// linkat/renameat), so a musl `rm`/`mkdir`/`ln` was silently misrouted to
// connect/listen/.... Relocated into the private range so the Linux numbers are
// free; native callers use the SYS_ macros below and recompile transparently.
#define SYS_SOCKET   (MYOS_SYS_BASE + 10) // x0=type -> fd
#define SYS_BIND     (MYOS_SYS_BASE + 11) // x0=fd, x1=port -> 0/-1
#define SYS_SENDTO   (MYOS_SYS_BASE + 12) // x0=fd, x1=buf, x2=len, x3=ip, x4=port -> bytes/-1
#define SYS_RECVFROM (MYOS_SYS_BASE + 13) // x0=fd, x1=buf, x2=len, x3=uint*ip, x4=uint16*port -> bytes/-1
#define SYS_CONNECT  (MYOS_SYS_BASE + 14) // x0=fd, x1=ip, x2=port -> 0/-1 (TCP handshake)
#define SYS_LISTEN   (MYOS_SYS_BASE + 15) // x0=fd, x1=backlog -> 0/-1 (passive open)
#define SYS_ACCEPT   (MYOS_SYS_BASE + 16) // x0=fd -> new connected fd, or -1
#define SYS_POLL     (MYOS_SYS_BASE + 17) // x0=pollfd*, x1=nfds, x2=timeout_ms -> #ready/0/-1
#define SYS_SOCKSHUT (MYOS_SYS_BASE + 18) // x0=fd, x1=how (SHUT_WR/...) -> 0/-1 (TCP half-close)
#define SYS_OPENPT   (MYOS_SYS_BASE + 19) // x0=int fd[2] -> fills {master, slave}; 0 / -1

// Terminal ioctls (asm-generic values) -- enough for isatty()/line editing.
// musl's isatty() calls ioctl(fd, TCGETS, &termios); a 0 return means the fd
// is a terminal. ash then uses TCSETS* to switch to raw (no-echo) mode, and
// TIOCGWINSZ / TIOCGPGRP for window size and foreground process group.
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410

// Dispatch the syscall described by the trap frame (number in x[8], args in
// x[0..]); write the result into x[0]. Returns the result too.
long do_syscall(struct trapframe *tf);
