// pty.c -- pseudo-terminal: two rings plus a real tty line discipline.
// ===================================================================
//
// See pty.h for the big picture. The interesting code is the line discipline in
// pty_master_write(): the same keystroke byte is treated completely differently
// depending on the slave's termios. A cooked shell wants line editing and ^C ->
// SIGINT; vi clears ICANON/ECHO/ISIG and then wants every raw byte delivered
// instantly so it can drive the screen itself.
//
// Blocking follows the pipe model (sched_block/sched_wake on a wait-channel),
// NOT the console's IRQ-masked model: a PTY has no interrupt -- both ends are
// driven by ordinary threads inside syscalls, so a plain sleep/wake is enough.
// The two ring-count fields double as wait-channels (their addresses are keys).

#include <stdint.h>
#include "pty.h"
#include "vfs.h"
#include "kheap.h"
#include "sched.h"
#include "signal.h"

// --- ring helpers -----------------------------------------------------------
// Single fixed-size byte ring per direction. Push drops on overflow (a real tty
// also discards input it has no room for); pop returns -1 when empty.
static void ring_push(char *buf, uint32_t *r, uint32_t *w, uint32_t *count, char c)
{
    (void)r;
    if (*count == PTY_BUF) { return; }       // full -> drop the byte
    buf[*w] = c;
    *w = (*w + 1) % PTY_BUF;
    (*count)++;
}

static int ring_pop(char *buf, uint32_t *r, uint32_t *w, uint32_t *count)
{
    (void)w;
    if (*count == 0) { return -1; }
    int c = (unsigned char)buf[*r];
    *r = (*r + 1) % PTY_BUF;
    (*count)--;
    return c;
}

struct pty *pty_alloc(void)
{
    struct pty *p = kmalloc(sizeof(struct pty));
    p->o_r = p->o_w = p->o_count = 0;
    p->i_r = p->i_w = p->i_count = 0;
    p->line_len = 0;
    // Default termios: a cooked terminal -- canonical input, echo on, signals
    // on, CR->NL on input, NL->CR-NL on output. These are the conventional
    // power-on defaults; a slave that wants raw mode (vi) clears the flags via
    // TCSETS, which lands in p->tio unchanged and immediately changes behaviour.
    p->tio.c_iflag = I_ICRNL;
    p->tio.c_oflag = O_OPOST | O_ONLCR;
    p->tio.c_cflag = 0;
    p->tio.c_lflag = T_ISIG | T_ICANON | T_ECHO;
    p->tio.c_line  = 0;
    for (int i = 0; i < 19; i++) { p->tio.c_cc[i] = 0; }
    p->tio.c_cc[VINTR]  = 3;    // ^C
    p->tio.c_cc[VQUIT]  = 28;   // QUIT (Ctrl-backslash)
    p->tio.c_cc[VERASE] = 127;  // DEL
    p->tio.c_cc[VEOF]   = 4;    // ^D
    p->tio.c_cc[VSUSP]  = 26;   // ^Z
    p->tio.c_ispeed = p->tio.c_ospeed = 0;
    p->ws.ws_row = p->ws.ws_col = p->ws.ws_xpix = p->ws.ws_ypix = 0;
    p->fg_pgrp = 0;
    p->master_open = 1;
    p->slave_open = 1;
    return p;
}

// Echo one byte to the OUTPUT ring (so the emulator paints what was typed) and
// wake a master reader waiting to render. Control chars other than NL are not
// expanded to ^X here -- cooked echo of printable text + newline is enough for
// the shells and line-readers Phase 1 targets.
static void echo(struct pty *p, char c)
{
    if (c == '\n') {
        ring_push(p->obuf, &p->o_r, &p->o_w, &p->o_count, '\r');
        ring_push(p->obuf, &p->o_r, &p->o_w, &p->o_count, '\n');
    } else {
        ring_push(p->obuf, &p->o_r, &p->o_w, &p->o_count, c);
    }
    sched_wake(&p->o_count);
}

// Commit the pending cooked line (including its terminating NL/EOF) into the
// input ring and wake the slave reader. Resets the edit buffer for the next line.
static void commit_line(struct pty *p)
{
    for (uint32_t i = 0; i < p->line_len; i++) {
        ring_push(p->ibuf, &p->i_r, &p->i_w, &p->i_count, p->line[i]);
    }
    p->line_len = 0;
    sched_wake(&p->i_count);
}

// Master writes keystrokes -> run them through the line discipline -> deliver to
// the slave (and echo to the screen). This is the core of "a PTY is not a pipe".
int pty_master_write(struct pty *p, const void *buf, uint64_t len)
{
    const unsigned char *b = (const unsigned char *)buf;
    int lflag = (int)p->tio.c_lflag;
    int iflag = (int)p->tio.c_iflag;
    for (uint64_t k = 0; k < len; k++) {
        unsigned char c = b[k];

        // Input mapping: CR -> NL when ICRNL (so RET reads as '\n').
        if (c == '\r' && (iflag & I_ICRNL)) { c = '\n'; }

        // Signal generation (ISIG). Only INTR is wired -- this kernel has no
        // SIGQUIT/SIGTSTP -- so ^C interrupts the foreground group and is
        // consumed (not echoed, not delivered as data). Everything else with
        // ISIG falls through to normal handling.
        if ((lflag & T_ISIG) && c == p->tio.c_cc[VINTR]) {
            if (p->fg_pgrp > 0) { sched_kill(-p->fg_pgrp, SIGINT); }
            continue;
        }

        if (lflag & T_ICANON) {
            // Cooked mode: assemble an editable line; deliver it whole on RET.
            if (c == p->tio.c_cc[VERASE] || c == '\b') {
                if (p->line_len > 0) {
                    p->line_len--;
                    if (lflag & T_ECHO) {       // erase on screen: back, blank, back
                        echo(p, '\b'); echo(p, ' '); echo(p, '\b');
                    }
                }
                continue;
            }
            if (c == p->tio.c_cc[VEOF]) {
                // ^D: deliver what's typed so far immediately (a line-reader sees
                // a short line; an empty line is full EOF in POSIX -- deferred).
                commit_line(p);
                continue;
            }
            if (c == '\n') {
                if (p->line_len < PTY_LINE) { p->line[p->line_len++] = '\n'; }
                if (lflag & T_ECHO) { echo(p, '\n'); }
                commit_line(p);
                continue;
            }
            if (p->line_len < PTY_LINE - 1) {   // ordinary char -> edit buffer
                p->line[p->line_len++] = (char)c;
                if (lflag & T_ECHO) { echo(p, (char)c); }
            }
            continue;
        }

        // Raw mode: every byte goes straight to the slave, unbuffered. Echo only
        // if the (unusual for raw) ECHO bit is set.
        ring_push(p->ibuf, &p->i_r, &p->i_w, &p->i_count, (char)c);
        if (lflag & T_ECHO) { echo(p, (char)c); }
        sched_wake(&p->i_count);
    }
    return (int)len;
}

// Slave reads committed keystrokes. Blocks until input is ready, or returns 0
// (EOF) once the master has closed and nothing is buffered.
int pty_slave_read(struct pty *p, void *buf, uint64_t len)
{
    while (p->i_count == 0 && p->master_open) { sched_block(&p->i_count); }
    char *out = (char *)buf;
    uint64_t n = 0;
    int c;
    while (n < len && (c = ring_pop(p->ibuf, &p->i_r, &p->i_w, &p->i_count)) >= 0) {
        out[n++] = (char)c;
    }
    return (int)n;                            // 0 only when empty + master closed
}

// Slave writes screen output toward the master. Honours OPOST|ONLCR so a cooked
// program's bare '\n' becomes '\r\n' (column reset + line feed) for the emulator.
int pty_slave_write(struct pty *p, const void *buf, uint64_t len)
{
    const char *b = (const char *)buf;
    int post = (p->tio.c_oflag & O_OPOST) && (p->tio.c_oflag & O_ONLCR);
    for (uint64_t k = 0; k < len; k++) {
        if (b[k] == '\n' && post) {
            ring_push(p->obuf, &p->o_r, &p->o_w, &p->o_count, '\r');
        }
        ring_push(p->obuf, &p->o_r, &p->o_w, &p->o_count, b[k]);
    }
    sched_wake(&p->o_count);
    return (int)len;
}

// Master reads the program's screen output. Blocks until output is ready, or
// returns 0 (EOF) once the slave has closed and nothing is buffered.
int pty_master_read(struct pty *p, void *buf, uint64_t len)
{
    while (p->o_count == 0 && p->slave_open) { sched_block(&p->o_count); }
    char *out = (char *)buf;
    uint64_t n = 0;
    int c;
    while (n < len && (c = ring_pop(p->obuf, &p->o_r, &p->o_w, &p->o_count)) >= 0) {
        out[n++] = (char)c;
    }
    return (int)n;
}

int pty_master_readable(struct pty *p) { return p->o_count > 0 || !p->slave_open; }
int pty_slave_readable (struct pty *p) { return p->i_count > 0 || !p->master_open; }

// Close one end (identified by f->writable: we reuse it as is_master, set when
// the master file is created). Wake the peer so a blocked read sees the hangup;
// free the PTY only when both ends are gone.
void pty_close(struct file *f)
{
    struct pty *p = f->pty;
    if (f->is_master) { p->master_open = 0; sched_wake(&p->i_count); }
    else              { p->slave_open  = 0; sched_wake(&p->o_count); }
    if (!p->master_open && !p->slave_open) { kfree(p); }
}
