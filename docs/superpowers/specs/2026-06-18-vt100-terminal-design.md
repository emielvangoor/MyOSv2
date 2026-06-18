# VT100 Terminal Emulator (`/bin/term`) — Design

**Date:** 2026-06-18
**Goal:** Run real full-screen TUIs (`vi`, `less`, `top`) by giving MyOSv2 a
genuine pseudo-terminal and a standalone terminal emulator built on **libvterm**.

## Problem

The frame deliberately is *not* a terminal: program output is inserted into a
Lisp buffer with SGR colours translated to faces, and every other escape
sequence (cursor addressing `ESC[H`, clear `ESC[2J`, scroll regions) is
**stripped**. `busybox vi` paints a 2-D screen by *addressing cells*, so with
those sequences gone its output collapses into stacked garbage. Worse, vi calls
`isatty(0)` then `tcsetattr` to flip the tty raw — but the frame talks to
children over **pipes** (not a tty) and `TCSETS` is a **no-op**, so vi can
neither detect a terminal nor disable line-buffering. Making vi work therefore
needs (a) a real PTY with enforced termios, and (b) a screen-grid emulator.

## Decision: standalone full-screen `/bin/term`, libvterm-backed

We build a separate userspace program (modelled on `user/lm.c` / `teapot`) that
owns a **seat**, runs `busybox sh` on a kernel **PTY**, drives a **libvterm**
screen model, and blits the cell grid to virtio-gpu using the existing
`font_aa` glyphs. It is *not* an Emacs buffer inside the frame — it lives on its
own seat (Ctrl-Alt-F1..F4 switches to it), which keeps the render path simple
(no buffer-mirror) at the cost of not composing with Lisp windows.

### Settled choices
- **Simplified `openpty()` syscall** returning `(master_fd . slave_fd)` — not a
  full `/dev/ptmx` + `/dev/pts/N` + `grantpt`/`ptsname` stack. `/bin/term` is
  the only allocator and the child inherits the slave on fd 0/1/2. Covers
  vi/less/top/htop/sh; defers `tmux`/`script` (which open their own ptys).
- **ASCII-only font first.** `font_aa` covers ASCII 32–126; box-drawing and
  Unicode render as `?`. vi/less/top/shell are fine; font extension is a later
  phase.
- **Raw key delivery for a seat owner.** `SYS_INPUT_READ` currently swallows
  `^C` for the active seat and calls the global `tty_intr()`. A terminal needs
  the keystroke to reach `/bin/term`, be forwarded to the PTY master, and be
  interpreted by *that PTY's* line discipline (SIGINT when cooked+ISIG, raw
  `0x03` byte when raw). So a seat owner opts into raw key delivery, suppressing
  the kernel `^C` capture, while `Ctrl-Alt-F1..F4` stays reserved for seat
  switching.

## Architecture (bottom-up)

### Layer 1 — Kernel PTY device (`src/pty.c`, `src/pty.h`)
A master/slave pair, plugged into the VFS the way the console device is. New
`struct pty` holds: per-PTY `termios`, `winsize`, foreground pgrp, and two byte
rings — **output** (slave writes → master reads, the program's screen paint) and
**input** (master writes → slave reads, the keystrokes). `struct file` gains a
`struct pty *pty` pointer and an `is_master` flag; `vfs_read`/`vfs_write`/
`vfs_close` dispatch on `->pty` like they do on `->pipe`.

**Line discipline (the heart of vi support)** lives on the master-write →
slave-read path:
- `ICANON`: cooked mode assembles a line in a pending buffer; ERASE (`^H`/DEL)
  edits it; the whole line (incl. `\n`) commits to the input ring on NL/EOL.
  Raw mode commits each byte immediately.
- `ECHO`: echo input bytes back to the **output** ring so the terminal shows
  typing.
- `ISIG`: INTR (`^C`)→SIGINT, QUIT (`^\`)→SIGQUIT, SUSP (`^Z`)→SIGTSTP to the
  PTY's foreground pgrp. Disabled in raw mode → byte delivered literally.
- Output path is raw passthrough (libvterm interprets `\n`/`\r` itself; no
  OPOST).

**Allocation:** `SYS_OPENPT` → kernel allocates a `struct pty`, two `struct
file`s (master + slave), installs them in the fd table, returns `(master .
slave)` (a cons, mirroring `SYS_PIPE`).

**ioctl refactor:** `SYS_IOCTL` currently mutates global console state. Route
`TCGETS`/`TCSETS*`, `TIOCGWINSZ`/`TIOCSWINSZ`, `TIOCGPGRP`/`TIOCSPGRP` to the
*fd's* PTY instance when `fds[fd]->pty` is set (fall back to the console
behaviour otherwise). `TCGETS` succeeding is what makes `isatty()` true; real
`TCSETS` storage is what lets vi go raw. `TIOCSWINSZ` from the master raises
`SIGWINCH` on the foreground pgrp.

This layer is pure mechanism and KTEST-first: open a pty, write the master,
assert the slave reads obey ICANON/ECHO/raw/ISIG.

### Layer 2 — `/bin/term` (libvterm + render + input)
Main loop like the frame's. Startup: `gfx_acquire()` a seat; `openpty()`; fork;
child does `setsid` + dup slave→0/1/2 + `setpgid`, sets `TERM`, execs
`busybox sh`; parent keeps the master.

- **Geometry:** 2560×1440 fb ÷ 20×40 cell = **128×36** grid → PTY winsize and
  `vterm_set_size(36,128)`.
- **Output:** poll master → `vterm_input_write(bytes)`; on libvterm's damage
  callback, read cells (`vterm_screen_get_cell`) → paint each via copied-in
  `blend()` + glyph blit from `font_aa` → `gfx_flush()` the dirty rect.
  libvterm's output callback (replies to `ESC[c`/`ESC[6n`) is written back to
  the master.
- **Input:** `input_read()` yields raw evdev keycodes + modifier state.
  `/bin/term` carries a keycode→key table and calls
  `vterm_keyboard_unichar` / `vterm_keyboard_key(VTERM_KEY_UP, mods)`; libvterm
  emits the correct bytes (`ESC[A`, …) which we write to the master. The bulk of
  layer-2 work is this table + modifier handling.

### Layer 3 — terminfo + `$TERM`
libvterm targets xterm semantics. Stage an `xterm` (or `vt100`) terminfo entry
onto the ext2 image and set `TERM=xterm` for the child. `busybox vi` is largely
fine on `vt100`; `less`/ncurses need the entry present.

## Build
Vendor libvterm under `user/libvterm/` with a `vterm_rt.c` libc shim (malloc +
`mem*`), a Makefile rule cloned from the TinyGL pattern, linked into
`term.elf`. Stage `term.elf` + terminfo onto the disk image.

## Phasing (TDD; each phase independently demoable)
1. **Kernel PTY + termios discipline** — KTESTs for ICANON/ECHO/raw/ISIG, the
   `openpty` syscall, and the ioctl routing. No graphics.
2. **`/bin/term` skeleton** — seat + PTY + shell; render the grid;
   printable + Enter + Backspace only → a working shell prompt on screen.
3. **Full input** — arrows/F-keys/modifiers/signal chars + raw-seat delivery →
   vi fully usable.
4. **terminfo/`$TERM` + polish** — cursor rendering, colour mapping, `SIGWINCH`,
   verify vi/less/top.
5. *(optional, later)* extend the font for box-drawing/Unicode.

## Out of scope (v1)
Full `/dev/ptmx` POSIX pty stack, Unicode/wide glyphs, mouse reporting,
alternate-screen scrollback history, `tmux`/`script`.
