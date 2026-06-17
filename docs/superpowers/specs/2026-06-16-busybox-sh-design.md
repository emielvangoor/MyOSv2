# Interactive busybox sh — Design

**Date:** 2026-06-16
**Status:** Approved (build immediately — minimal review gating per user)
**Goal:** Make `(run "busybox" "sh")` a usable **interactive** shell over the serial console: it shows a prompt, accepts line-edited input, resolves bare command names (`ls`, `cat`, …) to busybox applets, and Ctrl-C returns to the prompt — without the `[syscall] unhandled` console spam.

---

## Background / what already works

busybox is a prebuilt static-musl aarch64 multicall binary at `/bin/busybox`. Investigation showed:

- `busybox sh -c "…"` **already runs** scripts; `busybox <applet>` (e.g. `busybox ls /`) **already works** and lists the real ext2 root.
- Every `[syscall] unhandled` is **non-fatal** (`-ENOSYS`); busybox copes. Observed: `rt_sigaction(134)`, `getppid(173)` (shell start); `clock_gettime(113)`, `gettimeofday(169)`, `fcntl(25)` (ls).
- `(run "busybox" "sh")` (no `-c`) looks dead because **`ioctl` returns `-ENOTTY` for everything** → `isatty(0)` is false → ash runs non-interactive, prints **no prompt**, and silently blocks on `read(0)`.
- `read(fd 0)` already returns one char at a time from `console_getc()` (UART, blocking). Ctrl-C (0x03) in `console.c` already `signal_send(fg, SIGINT)`, and a waited-on child is `sched_foreground()` — so Ctrl-C already *targets* busybox; it just has no installed handler yet.
- The native syscall ABI is a **mix**: real aarch64 numbers for common calls, but custom low numbers for a few (`SYS_KILL=20`, `SYS_SIGNAL=21`, `SYS_SIGRETURN=22`, `SYS_SETPGID=44`). Real musl binaries use the **real** numbers (`kill=129`, `setpgid=154`, `rt_sigaction=134`, `rt_sigreturn=139`), which the kernel does not yet handle.

The four gaps below close the distance to a real interactive shell.

---

## Component 1 — TTY ioctls (the prompt unlock)

In `src/syscall.c`, replace the blanket `SYS_IOCTL → -ENOTTY` with handling for the terminal requests on the console fds (0/1/2). For any **other** fd or request, keep returning `-ENOTTY`.

Requests (asm-generic values, used by musl/busybox):

| Request | Value | Behavior |
|---|---|---|
| `TCGETS` | `0x5401` | Zero-fill the user `struct termios` at `arg`, set sane flags (see below), return 0. **This is what makes `isatty(0)` true → ash goes interactive.** |
| `TCSETS`/`TCSETSW`/`TCSETSF` | `0x5402`/`3`/`4` | Accept and return 0 (we ignore the modes — input is already raw char-at-a-time; ash drives its own line editing/echo). |
| `TIOCGWINSZ` | `0x5413` | Fill `struct winsize { u16 row,col,xpixel,ypixel }` at `arg` with `{24,80,0,0}`, return 0. |
| `TIOCGPGRP` | `0x540F` | Write the caller's pid to the `int*` at `arg`, return 0 (benign — single foreground group). |
| `TIOCSPGRP` | `0x5410` | Return 0 (accept; no real process groups to switch). |

`struct termios` (musl/asm-generic): `tcflag_t c_iflag,c_oflag,c_cflag,c_lflag; cc_t c_line; cc_t c_cc[19]; speed_t __c_ispeed,__c_ospeed;` — define a matching kernel struct. For `TCGETS` set reasonable defaults (`c_lflag = ICANON|ECHO` is fine; ash flips to raw via `TCSETS` which we accept). The exact bits don't matter for `isatty`; only the `return 0` does.

Guard the writes: only honor these when `fd` is 0/1/2 (or any open file — but realistically the console). Validate `arg` is non-NULL.

**Contingency (cooked mode):** if this busybox lacks built-in line editing and expects the *kernel* to echo and line-buffer (canonical mode), the prompt will appear but typed characters won't echo / lines won't assemble. If the integration test shows that, add minimal canonical handling for `read(fd 0)`: echo each char to the console, buffer until `\n`, and return the line. This is a fallback task, only if observed.

## Component 2 — process / time / fcntl syscalls

In `src/syscall.c`, add handlers at the **real aarch64 numbers** (add `#define`s to `src/syscall.h`):

| Syscall | Num | Behavior |
|---|---|---|
| `getppid` | 173 | Return the current thread's parent pid (`sched_current()->parent->...`; 1 or 0 if none). |
| `clock_gettime` | 113 | `arg0`=clockid, `arg1`=`struct timespec*`. Fill from the system tick/uptime (best-effort monotonic; reuse whatever `timer`/uptime source exists). Return 0. |
| `gettimeofday` | 169 | `arg0`=`struct timeval*`. Fill seconds/usec best-effort, return 0. |
| `fcntl` | 25 | `F_GETFD`(1)→0; `F_SETFD`(2)→0 (we don't track FD_CLOEXEC yet — accept); `F_GETFL`(3)→`O_RDWR`(2); `F_SETFL`(4)→0; `F_DUPFD`(0)/`F_DUPFD_CLOEXEC`(1030)→dup into the fd table at/above arg. Unknown cmd → 0 (lenient). |
| `kill` | 129 | Same as the existing `sched_kill` used by `SYS_KILL=20`, just at 129. |
| `setpgid` | 154 | Same as the existing handler at `SYS_SETPGID=44`, at 154. |

Keep the legacy custom numbers working too (the native programs still use them). These are additive `case`s sharing the same logic.

## Component 3 — rt_sigaction / rt_sigreturn (Ctrl-C)

In `src/syscall.c`:

- `rt_sigaction` (**134**): `arg0`=sig, `arg1`=`const struct sigaction* act`, `arg2`=`struct sigaction* oact`, `arg3`=sigsetsize. The kernel sigaction layout: offset 0 = handler, 8 = flags, 16 = restorer, 24 = mask. If `oact` non-NULL, write back the current handler. If `act` non-NULL: store `t->sig_handler[sig] = act->handler`; if `act->sa_restorer` is non-NULL, set `t->sig_tramp = restorer`. Return 0. (`SIG_IGN`=1 / `SIG_DFL`=0 handled like the existing machinery.)
- `rt_sigreturn` (**139**): identical to `SYS_SIGRETURN=22` (restore the saved trapframe from the user stack).

This reuses the existing `signals_deliver()` path (`ELR`=handler, `lr`=`t->sig_tramp`, frame saved on the user stack). When busybox's SIGINT handler runs and `ret`s, it lands on `t->sig_tramp` which calls `rt_sigreturn(139)`.

**Trampoline uncertainty + fallback:** on aarch64, musl may **not** pass `sa_restorer` (the kernel is expected to provide the return trampoline via the vDSO, which we lack). If `sa_restorer` is NULL, the handler has nowhere clean to return. Mitigation, in priority order:
1. If musl *does* pass a restorer → it just works.
2. Else, the kernel maps a tiny fixed trampoline (`mov x8, #139; svc #0`) into every user address space at a known VA and defaults `t->sig_tramp` to it. (A "vDSO-lite" — one read-only exec page.)
3. If neither is feasible in scope: `rt_sigaction` still **installs the handler and returns 0** (kills the console spam, lets sh start). Ctrl-C may not cleanly resume — acceptable as a known follow-up. The integration test asserts the prompt + applets; Ctrl-C resume is a *secondary* assertion that may be deferred.

Document whichever path is taken.

## Component 4 — symlinks (read path) in ext2 + VFS

Bare-name command resolution (`ls` → `/bin/ls` → symlink → `/bin/busybox`). Symlinks are **created host-side** by `mke2fs -d` from `ln -s` in `build/rootfs` (see Component 5), so the kernel needs only to **read and follow** them — no on-device symlink creation.

**VFS (`vfs.h`/`vfs.c`):**
- Add `VN_SYMLINK = 2` to `enum vnode_type`.
- Add to `vnode_ops`: `int (*readlink)(struct vnode *vn, char *buf, int len)` → writes the target path, returns its length.
- In `walk_from`, after `lookup` yields a component, if it is `VN_SYMLINK`, resolve it: `readlink` the target; if it starts with `/`, restart the walk from `root`; otherwise continue from the current directory with the target spliced in front of the remaining path. Cap at 8 hops (`-ELOOP`/return 0 beyond). Follow on **every** component including the final one (sufficient for exec; `O_NOFOLLOW` semantics are out of scope).

**ext2 (`ext2.c`/`ext2.h`):**
- `#define EXT2_S_IFLNK 0xA000`, `#define EXT2_FT_SYMLINK 7`.
- `mkvnode`/`ext2_lookup`: map `IFLNK` inodes to `VN_SYMLINK` vnodes.
- `ext2_readlink`: **fast symlink** — if `i_blocks == 0` (and size < 60), the target is stored inline in the `i_block[]` bytes (60 bytes); copy `i_size` bytes from there. **Slow symlink** — otherwise read block 0 of the file (target in a data block). Return length.
- No `ext2_symlink` (create) needed; on-device symlink creation is out of scope (note it).

`vfs_readlink(path,buf,len)` wrapper + (optional) a `SYS_READLINKAT` handler are **not required** for the goal (the kernel follows symlinks internally during lookup/exec). Add `readlinkat` only if busybox turns out to call it during normal shell use.

## Component 5 — install applet symlinks at build time

In the `Makefile` `disk.img` recipe, after staging `/bin/busybox`, create symlinks for a curated set of applets so `mke2fs -d` bakes them as ext2 symlinks:

```make
for a in ls cat echo pwd cp mv rm mkdir rmdir touch ln \
         grep sed head tail wc sort uniq cut tr find \
         ps kill sleep date uname clear true false env which \
         chmod df du dd more vi; do \
  ln -sf busybox $(BUILD)/rootfs/bin/$$a; done
```

(Relative target `busybox` so the link is valid inside the guest's `/bin`. The set is curated rather than busybox's full `--list` because the host can't run the aarch64 binary to enumerate applets; it covers the common interactive commands. Adding more later is one line.)

`check_rootfs_staging.sh` gains a couple of assertions that `/bin/ls` and `/bin/cat` exist as symlinks in `build/rootfs`.

---

## Testing

**TDD.** `make test` (KTESTs) gates every commit.

- **KTESTs (`src/tests.c`):**
  - ioctl: `TCGETS` on fd 0 returns 0 and `isatty`-style success; `TIOCGWINSZ` fills `{24,80}`; a non-tty request still `-ENOTTY`.
  - getppid/clock_gettime/gettimeofday/fcntl/kill@129/setpgid@154 return sane values via crafted trapframes.
  - rt_sigaction@134 installs a handler into `t->sig_handler[]` and returns 0; rt_sigreturn@139 restores a frame (mirror the existing SIGRETURN test).
  - ext2 symlink read: build a tiny in-test scenario OR rely on the disk image — add a fast-symlink fixture to `build/rootfs` (`ln -s busybox bin/ls`) and a KTEST that mounts ext2 at `/disk` (the existing test pattern) and asserts `vfs_lookup("/disk/bin/ls")` follows to the busybox regular-file vnode, and that a `VN_SYMLINK` vnode's `readlink` returns `"busybox"`.
- **Integration check (`tools/busyboxsh_check.py`, new):** boot, drive the serial REPL, `(run "busybox" "sh")`, assert a prompt appears (e.g. contains `$` or `/ #`), send `ls\n`, assert real root entries stream back (`usr`, `bin`), send `exit\n`, confirm return to `lisp>`. Optionally send Ctrl-C (0x03) at the prompt and assert the prompt returns (secondary; may be deferred per Component 3 fallback).
- **Existing checks** (`autostart`, `printf`, `persist`, KTEST suite) must stay green — symlink-following in `walk_from` must not regress normal path resolution.

---

## Out of scope

- On-device symlink **creation** (`symlink()` syscall, `ext2_symlink` write). Read+follow only.
- Full job control / process groups beyond benign `setpgid`/`pgrp` stubs.
- `O_NOFOLLOW`, symlink loops beyond a fixed hop cap, `/proc`, `/dev/tty`.
- Interactive busybox sh **inside the graphical frame** (input routing through the frame). Scope is the serial console, where the parent Lisp REPL blocks in `wait()` and hands the console to sh.
- Rebuilding the busybox binary (we use the committed `busybox.bin` as-is).
