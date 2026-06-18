// pty.h -- a pseudo-terminal: a bidirectional tty with a real line discipline.
// ===========================================================================
//
// A PTY is two coupled byte streams plus the terminal *behaviour* that a real
// tty has (which our raw UART console only fakes). It exists so full-screen
// programs -- `vi`, `less`, `top` -- can do what they expect of a terminal:
//
//   * isatty(0) is TRUE        (the slave answers TCGETS, unlike a pipe)
//   * tcsetattr() actually     (storing termios here REALLY switches the line
//     changes behaviour         discipline between cooked and raw -- our UART
//                               console ignores it)
//   * the screen is addressable (the program's cursor-control escapes flow
//                               through untouched for the emulator to interpret)
//
// Two ends, each a `struct file`:
//
//   master  <-- the terminal emulator (/bin/term).  Writes KEYSTROKES in,
//               reads the program's SCREEN OUTPUT out.
//   slave   <-- the program (busybox sh, vi).  Reads keystrokes (cooked or raw
//               by termios), writes screen output.
//
// Two rings carry the two directions:
//
//   input  ring:  master_write --[line discipline]--> slave_read   (keystrokes)
//   output ring:  slave_write  --------------------->  master_read  (screen)
//
// The line discipline lives on the master_write -> slave_read path and is the
// whole reason a PTY differs from a bare pipe: ICANON line-buffers and lets the
// user edit with ERASE before RET commits the line; ECHO bounces typed bytes
// back to the screen; ISIG turns ^C/^\/^Z into signals to the foreground group.
// A program that goes raw (vi) clears these bits and then sees every keystroke
// byte-at-a-time, unbuffered, unsignalled -- exactly what a screen editor needs.
#pragma once
#include <stdint.h>

#define PTY_BUF  4096     // per-direction ring capacity
#define PTY_LINE 256      // max pending canonical line (cooked-mode edit buffer)

// termios flag bits and control-char indices, matching the asm-generic / Linux
// aarch64 ABI (the same values musl's <termios.h> uses), so a slave's tcsetattr
// lands here unchanged.
#define T_ISIG    0x0001  // c_lflag: generate signals on INTR/QUIT/SUSP
#define T_ICANON  0x0002  // c_lflag: canonical (line-buffered, editable) input
#define T_ECHO    0x0008  // c_lflag: echo input back to the terminal
#define I_ICRNL   0x0100  // c_iflag: translate received CR to NL
#define O_OPOST   0x0001  // c_oflag: post-process output (we honour ONLCR only)
#define O_ONLCR   0x0004  // c_oflag: translate output NL to CR-NL

#define VINTR   0   // c_cc index: INTR  char (default ^C) -> SIGINT
#define VQUIT   1   // c_cc index: QUIT  char (default ^\) -> SIGQUIT
#define VERASE  2   // c_cc index: ERASE char (default DEL) -> backspace edit
#define VEOF    4   // c_cc index: EOF   char (default ^D) -> end-of-file
#define VSUSP   10  // c_cc index: SUSP  char (default ^Z) -> SIGTSTP

// Linux termios layout (asm-generic): 4 flag words, c_line, 19 control chars,
// then two speed words. We only act on the flags and c_cc, but keep the speeds
// so a TCGETS round-trips the struct a slave handed us via TCSETS.
struct pty_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line, c_cc[19];
    uint32_t c_ispeed, c_ospeed;
};

// struct winsize layout: rows, cols, x-pixels, y-pixels (all u16).
struct pty_winsize { uint16_t ws_row, ws_col, ws_xpix, ws_ypix; };

struct pty {
    // output ring: the program (slave) writes its screen paint here; the
    // emulator (master) reads it.
    char     obuf[PTY_BUF];
    uint32_t o_r, o_w, o_count;
    // input ring: COMMITTED keystrokes ready for the slave to read. In raw mode
    // each byte lands here immediately; in cooked mode a whole line lands at RET.
    char     ibuf[PTY_BUF];
    uint32_t i_r, i_w, i_count;
    // cooked-mode line under construction (not yet committed to ibuf). ERASE
    // edits it; NL/EOF flushes it into ibuf.
    char     line[PTY_LINE];
    uint32_t line_len;

    struct pty_termios tio;
    struct pty_winsize ws;
    int fg_pgrp;          // foreground process group (TIOCSPGRP) -- what ^C signals
    int master_open;      // 1 while the emulator holds the master end
    int slave_open;       // 1 while the program holds the slave end
};

struct file;   // from vfs.h (a PTY end is a file with ->pty set)

struct pty *pty_alloc(void);   // fresh PTY in canonical+echo+isig, 0x0 winsize

// Master end (the emulator). master_write feeds keystrokes through the line
// discipline; master_read drains the program's screen output (blocks for it).
int pty_master_write(struct pty *p, const void *buf, uint64_t len);
int pty_master_read (struct pty *p, void *buf, uint64_t len);

// Slave end (the program). slave_read pulls committed input (blocks for it);
// slave_write emits screen output toward the master.
int pty_slave_read (struct pty *p, void *buf, uint64_t len);
int pty_slave_write(struct pty *p, const void *buf, uint64_t len);

// Readiness predicates for poll() (non-blocking, no side effects).
int pty_master_readable(struct pty *p);  // program produced output to render
int pty_slave_readable (struct pty *p);  // a keystroke/line is ready for the program

// Drop one end; the PTY is freed when both ends are closed.
void pty_close(struct file *f);
