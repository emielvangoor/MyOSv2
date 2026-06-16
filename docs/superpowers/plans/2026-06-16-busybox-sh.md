# Interactive busybox sh — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `(run "busybox" "sh")` is a usable interactive shell over the serial console — shows a prompt, resolves bare command names to busybox applets via symlinks, no `[syscall] unhandled` spam, Ctrl-C returns to the prompt (best-effort).

**Architecture:** Add TTY ioctls so `isatty(0)` is true (ash goes interactive); add the real-numbered process/time/signal syscalls busybox uses; add **read-only** symlink support to ext2+VFS and bake `/bin/<applet> → busybox` symlinks into the disk image at build time.

**Tech Stack:** C freestanding aarch64 kernel; ext2 driver; GNU Make + `mke2fs -d` (bakes host `ln -s` as ext2 symlinks); prebuilt static-musl busybox; Python QEMU integration checks.

**Constraint:** Pre-commit hook runs `make test`; under `-DTEST_EXIT` kmain exits before mounting/spawn, so KTESTs can't exercise the real boot — but the tree must compile and KTESTs pass on every commit. Reference: `docs/superpowers/specs/2026-06-16-busybox-sh-design.md`.

---

## File Structure

| File | Change |
|---|---|
| `src/syscall.h` | `#define`s for new syscall numbers (134,139,173,113,169,25,129,154) + ioctl/fcntl constants |
| `src/syscall.c` | TTY ioctls; getppid/clock_gettime/gettimeofday/fcntl/kill@129/setpgid@154; rt_sigaction@134/rt_sigreturn@139 |
| `src/vfs.h`/`src/vfs.c` | `VN_SYMLINK`; `readlink` vnode op; symlink-following in `walk_from`; `vfs_readlink` |
| `src/ext2.h`/`src/ext2.c` | `EXT2_S_IFLNK`/`EXT2_FT_SYMLINK`; symlink vnodes; `ext2_readlink` (fast+slow) |
| `Makefile` | `ln -s busybox` applet symlinks in `rootfs/bin` |
| `tools/check_rootfs_staging.sh` | assert `/bin/ls`,`/bin/cat` symlinks staged |
| `src/tests.c` | KTESTs for ioctl, new syscalls, rt_sigaction, ext2 symlink follow |
| `tools/busyboxsh_check.py` | new integration check |

---

## Task 1: TTY ioctls — make ash go interactive

**Files:** Modify `src/syscall.c` (the `SYS_IOCTL` case), `src/syscall.h` (ioctl constants); Test `src/tests.c`.

- [ ] **Step 1: Add ioctl constants to `src/syscall.h`**

```c
// Terminal ioctls (asm-generic values) -- enough for isatty()/line editing.
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
```

- [ ] **Step 2: Write the failing KTEST**

In `src/tests.c`, add (and register in the table) a test that drives `SYS_IOCTL` via a trapframe. `TCGETS` on fd 0 must return 0 (success); a non-terminal request must return `-ENOTTY`:

```c
static void test_ioctl_tcgets_is_tty(void)
{
    pmm_init(); kheap_init();
    char termios_buf[64] = {0};
    struct trapframe tf = {0};
    tf.x[8] = SYS_IOCTL; tf.x[0] = 0; tf.x[1] = TCGETS; tf.x[2] = (uint64_t)(uintptr_t)termios_buf;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);                 // success -> isatty(0) true
    tf.x[8] = SYS_IOCTL; tf.x[0] = 0; tf.x[1] = 0xDEAD; tf.x[2] = 0;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == -ENOTTY);           // unknown request still ENOTTY
}
```

Register: `{ "ioctl: TCGETS is a tty", test_ioctl_tcgets_is_tty },`

- [ ] **Step 3: Run it — verify it fails**

Run: `make test` → expect the new test to FAIL (current ioctl returns `-ENOTTY` for TCGETS too).

- [ ] **Step 4: Implement the ioctl handling**

Replace the `SYS_IOCTL` case body in `src/syscall.c`. Define a kernel `struct termios`/`struct winsize` near the top of the file (or in a small header). Handle the requests:

```c
    case SYS_IOCTL: {                        // x0=fd, x1=request, x2=arg
        uint64_t fd = tf->x[0]; unsigned req = (unsigned)tf->x[1];
        void *arg = (void *)(uintptr_t)tf->x[2];
        switch (req) {
        case TCGETS:
            if (arg) {
                struct termios { unsigned int c_iflag,c_oflag,c_cflag,c_lflag;
                                 unsigned char c_line, c_cc[19]; unsigned int c_ispeed,c_ospeed; } t = {0};
                t.c_lflag = 0x0002 /*ICANON*/ | 0x0008 /*ECHO*/;   // cosmetic defaults
                *(struct termios *)arg = t;
            }
            ret = 0; break;                  // success => isatty(fd) is true
        case TCSETS: case TCSETSW: case TCSETSF:
            ret = 0; break;                  // accept; ash drives raw-mode editing itself
        case TIOCGWINSZ:
            if (arg) { unsigned short *ws = (unsigned short *)arg; ws[0]=24; ws[1]=80; ws[2]=0; ws[3]=0; }
            ret = 0; break;
        case TIOCGPGRP:
            if (arg) { *(int *)arg = sched_current() ? sched_current_pid() : 1; }
            ret = 0; break;
        case TIOCSPGRP:
            ret = 0; break;
        default:
            ret = -ENOTTY; break;            // unchanged: musl falls back to buffered I/O
        }
        break;
    }
```

Use the existing helper for the current pid (`sched_current_pid()` if present, else the thread's pid field — read `src/sched.h`). If a `struct termios`/`winsize` already exists elsewhere, reuse it instead of redefining.

- [ ] **Step 5: Run it — verify pass**

Run: `make test` → the new test passes; suite green (`164+1 passed`).

- [ ] **Step 6: Commit**

```bash
git add src/syscall.c src/syscall.h src/tests.c
git commit -m "feat(tty): handle terminal ioctls (TCGETS/TCSETS/winsz) so isatty() is true"
```

---

## Task 2: process / time / fcntl syscalls

**Files:** Modify `src/syscall.h` (numbers + fcntl consts), `src/syscall.c`; Test `src/tests.c`.

- [ ] **Step 1: Add syscall numbers + fcntl constants to `src/syscall.h`**

```c
#define SYS_FCNTL          25   // x0=fd, x1=cmd, x2=arg
#define SYS_CLOCK_GETTIME  113
#define SYS_KILL_LINUX     129  // real aarch64 kill (legacy SYS_KILL=20 stays)
#define SYS_SETPGID_LINUX  154  // real aarch64 setpgid (legacy SYS_SETPGID=44 stays)
#define SYS_GETTIMEOFDAY   169
#define SYS_GETPPID        173
// fcntl cmds
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
```

- [ ] **Step 2: Write failing KTESTs**

Add tests (register each) covering getppid, fcntl(F_GETFL), clock_gettime, and that kill@129 / setpgid@154 dispatch like their legacy twins:

```c
static void test_getppid_returns_parent(void)
{
    pmm_init(); kheap_init();
    struct trapframe tf = {0};
    tf.x[8] = SYS_GETPPID;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] >= 0);                 // a pid (0/1 in test context is fine)
}
static void test_fcntl_getfl_rdwr(void)
{
    pmm_init(); kheap_init();
    struct trapframe tf = {0};
    tf.x[8] = SYS_FCNTL; tf.x[0] = 0; tf.x[1] = F_GETFL; tf.x[2] = 0;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 2);                 // O_RDWR
}
static void test_clock_gettime_ok(void)
{
    pmm_init(); kheap_init();
    long ts[2] = {0,0};
    struct trapframe tf = {0};
    tf.x[8] = SYS_CLOCK_GETTIME; tf.x[0] = 1 /*MONOTONIC*/; tf.x[1] = (uint64_t)(uintptr_t)ts;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);
}
```

Register all three.

- [ ] **Step 3: Run — verify fail** (`make test`): these return `-ENOSYS` today.

- [ ] **Step 4: Implement the handlers in `src/syscall.c`**

Add cases. For getppid, read the parent pid from the scheduler (read `src/sched.h` for the exact field — likely `sched_current()->parent` then its pid field, e.g. `->tid`/`->pid`; guard NULL → return 1):

```c
    case SYS_GETPPID: {
        struct thread *t = sched_current();
        ret = (t && t->parent) ? t->parent->PIDFIELD : 1;   // use the real field name
        break;
    }
    case SYS_FCNTL: {
        int cmd = (int)tf->x[1];
        switch (cmd) {
        case F_GETFD: ret = 0; break;
        case F_SETFD: ret = 0; break;            // FD_CLOEXEC not tracked yet
        case F_GETFL: ret = 2 /*O_RDWR*/; break;
        case F_SETFL: ret = 0; break;
        default:      ret = 0; break;            // lenient
        }
        break;
    }
    case SYS_CLOCK_GETTIME: {
        long *ts = (long *)(uintptr_t)tf->x[1];  // [0]=sec, [1]=nsec
        if (ts) { uint64_t ms = uptime_ms(); ts[0] = (long)(ms/1000); ts[1] = (long)((ms%1000)*1000000); }
        ret = 0; break;
    }
    case SYS_GETTIMEOFDAY: {
        long *tv = (long *)(uintptr_t)tf->x[0];  // [0]=sec, [1]=usec
        if (tv) { uint64_t ms = uptime_ms(); tv[0] = (long)(ms/1000); tv[1] = (long)((ms%1000)*1000); }
        ret = 0; break;
    }
    case SYS_KILL_LINUX:                          // share legacy SYS_KILL logic
        ret = sched_kill((int)tf->x[0], (int)tf->x[1]); break;
    case SYS_SETPGID_LINUX:                        // share legacy SYS_SETPGID logic
        ret = /* same call the SYS_SETPGID case makes */ 0; break;
```

Use the real uptime source (read `src/timer.h`/`src/sched.h` for an existing `uptime_ms()`/tick counter; if none, use the tick count × ms-per-tick). For `SYS_SETPGID_LINUX`, call exactly what the existing `SYS_SETPGID=44` case calls (factor it if needed). Place `SYS_KILL_LINUX`/`SYS_SETPGID_LINUX` cases adjacent to their legacy twins.

- [ ] **Step 5: Run — verify pass** (`make test`).

- [ ] **Step 6: Commit**

```bash
git add src/syscall.c src/syscall.h src/tests.c
git commit -m "feat(syscall): getppid/clock_gettime/gettimeofday/fcntl + real-numbered kill/setpgid"
```

---

## Task 3: rt_sigaction / rt_sigreturn (Ctrl-C)

**Files:** Modify `src/syscall.h`, `src/syscall.c`; Test `src/tests.c`.

- [ ] **Step 1: Add numbers to `src/syscall.h`**

```c
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGRETURN 139
```

- [ ] **Step 2: Write the failing KTEST**

Mirror the existing SIGNAL test: rt_sigaction installs a handler into `t->sig_handler[]` and returns 0.

```c
static void test_rt_sigaction_installs(void)
{
    pmm_init(); kheap_init(); sched_init();
    struct thread *t = sched_current();
    // struct sigaction: [0]=handler, [1]=flags, [2]=restorer, [3..]=mask (each 8 bytes)
    uint64_t act[8] = {0};
    act[0] = 0x1234;                 // pretend handler addr
    act[2] = 0x5678;                 // restorer
    struct trapframe tf = {0};
    tf.x[8] = SYS_RT_SIGACTION; tf.x[0] = SIGINT;
    tf.x[1] = (uint64_t)(uintptr_t)act; tf.x[2] = 0; tf.x[3] = 8;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);
    KASSERT((uint64_t)(uintptr_t)t->sig_handler[SIGINT] == 0x1234);
    KASSERT(t->sig_tramp == 0x5678);
}
```

(Adjust to how `sched_current()` is established in the KTEST environment — follow the existing `test_*signal*` setup.)

- [ ] **Step 3: Run — verify fail** (`make test`).

- [ ] **Step 4: Implement in `src/syscall.c`**

```c
    case SYS_RT_SIGACTION: {                  // x0=sig, x1=const act*, x2=oact*, x3=sigsetsize
        struct thread *t = sched_current();
        int sig = (int)tf->x[0];
        const uint64_t *act = (const uint64_t *)(uintptr_t)tf->x[1];   // [0]=handler,[2]=restorer
        uint64_t *oact = (uint64_t *)(uintptr_t)tf->x[2];
        if (!t || sig <= 0 || sig >= 32) { ret = -EINVAL; break; }
        if (oact) { oact[0] = (uint64_t)(uintptr_t)t->sig_handler[sig]; oact[2] = t->sig_tramp; }
        if (act) {
            t->sig_handler[sig] = (uint64_t (*)(int))(uintptr_t)act[0];
            if (act[2]) { t->sig_tramp = act[2]; }   // sa_restorer, if musl supplies one
        }
        ret = 0; break;
    }
    case SYS_RT_SIGRETURN: {                  // identical to SYS_SIGRETURN
        const uint64_t *saved = (const uint64_t *)(uintptr_t)tf->sp_el0;
        uint64_t *d = (uint64_t *)tf;
        for (unsigned i = 0; i < sizeof(struct trapframe) / 8; i++) { d[i] = saved[i]; }
        ret = (long)tf->x[0];
        break;
    }
```

If, during the Task 6 integration test, Ctrl-C does **not** cleanly resume because musl passed no restorer (`t->sig_tramp` stays 0), apply the spec's fallback (a kernel-mapped trampoline page defaulted into `t->sig_tramp`). Otherwise leave as-is: handlers install and the console is clean, which is the primary win.

- [ ] **Step 5: Run — verify pass** (`make test`).

- [ ] **Step 6: Commit**

```bash
git add src/syscall.c src/syscall.h src/tests.c
git commit -m "feat(signal): rt_sigaction/rt_sigreturn -> existing delivery machinery (Ctrl-C in busybox)"
```

---

## Task 4: read-only symlinks in ext2 + VFS

**Files:** Modify `src/vfs.h`, `src/vfs.c`, `src/ext2.h`, `src/ext2.c`; Test `src/tests.c`.

- [ ] **Step 1: VFS plumbing**

In `src/vfs.h`: `enum vnode_type { VN_FILE=0, VN_DIR=1, VN_SYMLINK=2 };`. Add to `struct vnode_ops`: `int (*readlink)(struct vnode *vn, char *buf, int len);`. Declare `int vfs_readlink(struct vnode *vn, char *buf, int len);`.

In `src/vfs.c`: add `vfs_readlink` (calls the op, `-1` if absent). Modify `walk_from` to follow symlinks. After `cur = cur->ops->lookup(...)`, if `cur && cur->type == VN_SYMLINK`, resolve:

```c
// follow symlinks (read-only): splice the target in front of the rest of the path.
int hops = 0;
while (cur && cur->type == VN_SYMLINK) {
    if (++hops > 8) { return 0; }                 // loop guard (-ELOOP)
    char target[128];
    int n = vfs_readlink(cur, target, sizeof(target) - 1);
    if (n <= 0) { return 0; }
    target[n] = '\0';
    struct vnode *base = (target[0] == '/') ? root : dir_of_current_walk; // see note
    cur = walk_from(base, target[0] == '/' ? target + 1 : target);
}
```

Note: `walk_from` resolves one component at a time; the simplest correct implementation follows a symlink **only when it is the final resolved component** (which is the exec case: `/bin/ls`). For an intermediate symlink dir, follow then continue. Implement the final-component follow first (covers the goal), with the hop cap; keep the relative-base = the directory the symlink lives in. Recursion via `walk_from`/`vfs_lookup` is acceptable given the hop cap. Absolute targets restart from `root`.

- [ ] **Step 2: ext2 symlink read**

In `src/ext2.h`: `#define EXT2_S_IFLNK 0xA000`, `#define EXT2_FT_SYMLINK 7`.

In `src/ext2.c`:
- Where inode mode → vnode type is decided (in `mkvnode`/`ext2_lookup`), map `(i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK` to `VN_SYMLINK`.
- Add `ext2_readlink(struct vnode *vn, char *buf, int len)`:
  - Load the inode. Let `sz = i_size`.
  - **Fast symlink:** `if (ino.i_blocks == 0)` the target bytes live inline in `i_block[]` (treat `&i_block[0]` as up to 60 chars). Copy `min(sz, len)` bytes from there.
  - **Slow symlink:** else read file block 0 (via the existing block-map/read path) and copy `min(sz, len)` bytes.
  - Return the number of bytes copied.
- Wire `.readlink = ext2_readlink` into the ext2 `vnode_ops`.

- [ ] **Step 3: Test fixture + failing KTEST**

Add a symlink to the staging dir so the disk image carries one: in the `Makefile` `disk.img` recipe add `ln -sf busybox $(BUILD)/rootfs/bin/ls` (Task 5 generalizes this; add at least `ls` now). Then a KTEST mounting ext2 at `/disk` (existing pattern) that asserts the follow:

```c
static void test_ext2_symlink_follows(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());
    struct vnode *via = vfs_lookup("/disk/bin/ls");      // symlink -> busybox
    struct vnode *real = vfs_lookup("/disk/bin/busybox");
    KASSERT(via != 0 && real != 0);
    KASSERT(via == real || via->type == VN_FILE);        // followed to the regular file
}
```

(If `vfs_lookup` returns distinct vnode objects for the same inode, assert `via->type == VN_FILE` and same size; the key is that the symlink was followed, not returned as `VN_SYMLINK`.)

- [ ] **Step 4: Run — verify fail, then pass** (`make test`). It fails before the ext2/vfs changes (lookup returns the symlink vnode or NULL), passes after.

- [ ] **Step 5: Commit**

```bash
git add src/vfs.h src/vfs.c src/ext2.h src/ext2.c src/tests.c Makefile
git commit -m "feat(fs): read-only ext2 symlinks + VFS symlink-following in path resolution"
```

---

## Task 5: install applet symlinks at build time

**Files:** Modify `Makefile` (disk.img recipe), `tools/check_rootfs_staging.sh`.

- [ ] **Step 1: Extend the staging test**

In `tools/check_rootfs_staging.sh`, add `bin/ls` and `bin/cat` to the checked list, and assert they are symlinks:

```bash
for l in bin/ls bin/cat; do
  if [ ! -L "$R/$l" ]; then echo "NOT A SYMLINK: $R/$l"; fail=1; fi
done
```

- [ ] **Step 2: Run — verify fail** (`tools/check_rootfs_staging.sh` → fails: only `bin/ls` exists from Task 4, `bin/cat` and the symlink-ness of the full set are missing).

- [ ] **Step 3: Add the applet symlink loop to the `disk.img` recipe**

In the `Makefile` `$(BUILD)/disk.img` recipe, right after the busybox copy in the `/bin` staging block, add:

```make
	# busybox applet names: symlinks -> busybox so bare `ls`/`cat`/... resolve.
	# mke2fs -d bakes these as real ext2 symlinks; the kernel follows them.
	for a in ls cat echo pwd cp mv rm mkdir rmdir touch ln \
	         grep sed head tail wc sort uniq cut tr find xargs \
	         ps kill sleep date uname clear true false env which \
	         chmod df du dd more vi basename dirname seq yes tee; do \
	  ln -sf busybox $(BUILD)/rootfs/bin/$$a; done
```

(Relative target `busybox`. This replaces the single `ln -sf busybox .../bin/ls` added in Task 4 — keep the loop, drop the one-off.)

- [ ] **Step 4: Run — verify pass**

Run: `rm -f build/disk.img && tools/check_rootfs_staging.sh` → `rootfs staging OK`.

- [ ] **Step 5: `make test` stays green; commit**

Run: `make test`. Then:

```bash
git add Makefile tools/check_rootfs_staging.sh
git commit -m "build(fs): install busybox applet symlinks (/bin/ls -> busybox, ...) on the disk image"
```

---

## Task 6: integration test + docs

**Files:** Create `tools/busyboxsh_check.py`; modify `README.md`.

- [ ] **Step 1: Write `tools/busyboxsh_check.py`**

Boot via the harness, drive the serial REPL, and verify the interactive shell. Adapt the `Qemu` API (`expect`/`send_line`/`kill`) by reading `tools/lm_harness.py`.

```python
#!/usr/bin/env python3
"""
busyboxsh_check.py -- (run "busybox" "sh") is a usable interactive shell:
a prompt appears (TTY ioctls), `ls` resolves via the /bin symlinks to the
busybox applet and lists the real ext2 root, and `exit` returns to the Lisp REPL.
"""
import sys
sys.path.insert(0, "tools")
from lm_harness import Qemu

def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no REPL"); return 1
        q.send_line('(run "busybox" "sh")')
        # ash interactive prompt (FEATURE_SH default is like "/ # " or "~ $").
        if not (q.expect(b"#", 15) or q.expect(b"$", 5)):
            print("FAIL: no shell prompt -- isatty/TTY ioctls not working"); return 1
        q.send_line('ls /')
        if not q.expect(b"usr", 10):
            print("FAIL: `ls` did not resolve to the busybox applet / no output"); return 1
        q.send_line('exit')
        if not q.expect(b"lisp> ", 10):
            print("FAIL: did not return to the Lisp REPL after exit"); return 1
        print("BUSYBOX-SH OK")
        return 0
    finally:
        q.kill()

if __name__ == "__main__":
    sys.exit(main())
```

If the prompt-match is flaky (ash's prompt string varies), match on the post-`ls` output (`usr`/`bin`) as the primary signal of an interactive, applet-resolving shell. Tune against the real boot.

- [ ] **Step 2: Run it — iterate to green**

Run: `make build/kernel.elf build/disk.img >/dev/null && python3 tools/busyboxsh_check.py` → `BUSYBOX-SH OK`. If `ls` says "not found", the symlink follow (Task 4) or staging (Task 5) is off — debug there. If no prompt, revisit Task 1 (and the cooked-mode contingency in the spec). If Ctrl-C is in scope and fails, apply the Task 3 fallback.

- [ ] **Step 3: README**

Add a short bullet to `README.md` near the userland/filesystem section: busybox `sh` runs interactively over the console (`(run "busybox" "sh")`), with applets resolved by name via `/bin` symlinks; note ext2 now supports (read-only) symlinks.

- [ ] **Step 4: Commit**

```bash
git add tools/busyboxsh_check.py README.md
git commit -m "test(busybox): interactive busybox sh check (prompt + applet symlinks); README"
```

---

## Final verification

- [ ] `make clean && make test` green (KTESTs).
- [ ] `python3 tools/busyboxsh_check.py` → `BUSYBOX-SH OK`.
- [ ] `python3 tools/autostart_check.py`, `printf_check.py`, `persist_check.py` still green (symlink-following didn't regress path resolution; the frame still autostarts).
- [ ] No new `[syscall] unhandled` lines when running `busybox sh` (capture serial as in exploration).
- [ ] Final whole-implementation review, then `superpowers:finishing-a-development-branch`.
