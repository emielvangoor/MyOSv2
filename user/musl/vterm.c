// vterm.c -- /bin/vterm: a libvterm terminal that the frame drives over pipes.
// ============================================================================
//
// MyOSv2 is a Lisp machine, not a Unix box -- so a foreign full-screen program
// (busybox sh, vi) runs in a *buffer*, the Emacs way (M-x vterm). This is the
// "vterm helper": a tiny musl process that owns the messy POSIX half so the
// Lisp frame never has to. It opens a pseudo-terminal, runs a shell on it,
// drives **libvterm** (the VT100/xterm screen model), and speaks a line
// protocol over stdin/stdout to the frame:
//
//   frame -> us (stdin):    keys + resize          us -> frame (stdout): screen
//     u <codepoint> <mod>     a unicode key          z <rows> <cols>      geometry
//     k <keynum> <mod>        a named key (arrows…)  p <r> <c> <a> <fg> <bg> <text>
//     s <rows> <cols>         (re)size                                    a cell run
//     q                       quit                   c <row> <col> <vis>  cursor
//                                                     f                    end of batch
//                                                     x                    shell exited
//
//   <mod> bitmask: 1=Shift 2=Alt 4=Ctrl   (VTermModifier)
//   <keynum>: a VTermKey value (see vterm_keycodes.h)
//   <a> attrs bitmask: 1=bold 2=underline 4=reverse
//   <fg>/<bg>: -1 default, else a 0..15 ANSI palette index
//   <text>: the run's characters (ASCII; may contain spaces; never a newline)
//
// The frame mirrors each `p` run into a buffer with the matching ANSI face, and
// moves point to the `c` cursor. Keys it sends here we hand to libvterm's
// encoder, which writes the right bytes to the pty. The frame stays pure Lisp;
// all the libvterm/pty machinery lives in this one isolated binary.

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "vterm.h"

#define SYS_OPENPT 0x1013                  // MyOSv2-native: x0=int fd[2] -> 0/-1
static long svc1(long n, long a)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = 0, x2 __asm__("x2") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static int openpt(int fd[2]) { return (int)svc1(SYS_OPENPT, (long)fd); }

static int ROWS = 24, COLS = 80;
static int g_master;                       // pty master fd
static VTerm *vt;
static VTermScreen *vs;

static int cur_row, cur_col, cur_vis = 1;
static unsigned char *dirty;               // one flag per row

// --- output helpers ----------------------------------------------------------
// A whole protocol BATCH (one screen update) is buffered here and written with a
// single write() at the end, so the frame always reads it as a contiguous, line-
// aligned chunk -- never a half-written line interleaved with the helper's many
// small emits (which previously stranded a partial line in the pipe forever).
static char obuf[131072];
static int  olen;
static void oflush(void)
{
    const char *s = obuf; int n = olen; olen = 0;
    while (n > 0) { long k = write(1, s, n); if (k <= 0) break; s += k; n -= (int)k; }
}
static void wr(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (olen >= (int)sizeof(obuf)) { oflush(); }   // batch bigger than buffer: flush
        obuf[olen++] = s[i];
    }
}
static void wrs(const char *s) { wr(s, (int)strlen(s)); }
static void wrint(int v)
{
    char b[16]; int i = 0, neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (u == 0) { b[i++] = '0'; } else { while (u) { b[i++] = '0' + (u % 10); u /= 10; } }
    if (neg) { b[i++] = '-'; }
    char o[16]; int j = 0; while (i > 0) { o[j++] = b[--i]; } wr(o, j);
}

// --- libvterm callbacks ------------------------------------------------------
// libvterm wants to send bytes back to the program (query replies, key
// encodings) -> forward them to the shell via the pty master.
static void out_cb(const char *s, size_t len, void *user)
{
    (void)user;
    if (len > 0) { (void)!write(g_master, s, len); }
}
static int cb_damage(VTermRect rect, void *user)
{
    (void)user;
    for (int r = rect.start_row; r < rect.end_row && r < ROWS; r++) {
        if (r >= 0) { dirty[r] = 1; }
    }
    return 1;
}
static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    (void)src; (void)user;                 // a scroll: repaint the destination rows
    for (int r = dest.start_row; r < dest.end_row && r < ROWS; r++) {
        if (r >= 0) { dirty[r] = 1; }
    }
    return 1;
}
static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    (void)old; (void)user;
    cur_row = pos.row; cur_col = pos.col; cur_vis = visible;
    return 1;
}
static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    (void)user;
    if (prop == VTERM_PROP_CURSORVISIBLE) { cur_vis = val->boolean; }
    return 1;
}
static const VTermScreenCallbacks scr_cbs = {
    .damage = cb_damage, .moverect = cb_moverect,
    .movecursor = cb_movecursor, .settermprop = cb_settermprop,
};

// Map a libvterm colour to our emit code: -1 default, else 0..15 ANSI index.
// (256-colour / truecolour downsample to default for v1 -- attrs still apply.)
static int color_code(const VTermColor *c)
{
    if (VTERM_COLOR_IS_DEFAULT_FG(c) || VTERM_COLOR_IS_DEFAULT_BG(c)) { return -1; }
    if (VTERM_COLOR_IS_INDEXED(c)) { return c->indexed.idx < 16 ? c->indexed.idx : -1; }
    return -1;
}

// Emit every dirty row as runs of same-style cells, then the cursor, then `f`.
static void flush_damage(void)
{
    for (int r = 0; r < ROWS; r++) {
        if (!dirty[r]) { continue; }
        dirty[r] = 0;
        int c = 0;
        while (c < COLS) {
            VTermPos p = { .row = r, .col = c };
            VTermScreenCell cell;
            vterm_screen_get_cell(vs, p, &cell);
            int attrs = (cell.attrs.bold ? 1 : 0) | (cell.attrs.underline ? 2 : 0)
                      | (cell.attrs.reverse ? 4 : 0);
            int fg = color_code(&cell.fg), bg = color_code(&cell.bg);
            // collect a run with identical style
            char run[256]; int rn = 0, start = c;
            while (c < COLS && rn < (int)sizeof(run) - 1) {
                VTermPos q = { .row = r, .col = c };
                VTermScreenCell cc;
                vterm_screen_get_cell(vs, q, &cc);
                int a2 = (cc.attrs.bold ? 1 : 0) | (cc.attrs.underline ? 2 : 0)
                       | (cc.attrs.reverse ? 4 : 0);
                if (a2 != attrs || color_code(&cc.fg) != fg || color_code(&cc.bg) != bg) { break; }
                uint32_t ch = cc.chars[0] ? cc.chars[0] : ' ';
                run[rn++] = (ch >= 32 && ch < 127) ? (char)ch : (ch == 0 ? ' ' : '?');
                c++;
            }
            run[rn] = 0;
            wrs("p "); wrint(r); wrs(" "); wrint(start); wrs(" "); wrint(attrs);
            wrs(" "); wrint(fg); wrs(" "); wrint(bg); wrs(" "); wr(run, rn); wrs("\n");
        }
    }
    wrs("c "); wrint(cur_row); wrs(" "); wrint(cur_col); wrs(" "); wrint(cur_vis); wrs("\n");
    wrs("f\n");
    oflush();                                  // one contiguous write per batch
}

// --- command parsing (one line of frame->us protocol) ------------------------
static void handle_command(char *line)
{
    if (line[0] == 'q') { exit(0); }
    if (line[0] == 'u' || line[0] == 'k') {
        int a = 0, b = 0; char *s = line + 1;
        a = (int)strtol(s, &s, 10);
        b = (int)strtol(s, &s, 10);
        if (line[0] == 'u') { vterm_keyboard_unichar(vt, (uint32_t)a, (VTermModifier)b); }
        else                { vterm_keyboard_key(vt, (VTermKey)a, (VTermModifier)b); }
    } else if (line[0] == 's') {
        char *s = line + 1;
        int rows = (int)strtol(s, &s, 10), cols = (int)strtol(s, &s, 10);
        if (rows > 0 && cols > 0 && rows <= 200 && cols <= 400) {
            ROWS = rows; COLS = cols;
            struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
            ioctl(g_master, TIOCSWINSZ, &ws);
            vterm_set_size(vt, rows, cols);
            free(dirty); dirty = calloc((size_t)rows, 1);
            wrs("z "); wrint(ROWS); wrs(" "); wrint(COLS); wrs("\n");
            for (int r = 0; r < ROWS; r++) { dirty[r] = 1; }
            flush_damage();
        }
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3) { ROWS = atoi(argv[1]); COLS = atoi(argv[2]); }
    if (ROWS <= 0 || ROWS > 200) { ROWS = 24; }
    if (COLS <= 0 || COLS > 400) { COLS = 80; }
    dirty = calloc((size_t)ROWS, 1);

    int fd[2];
    if (openpt(fd) != 0) { wrs("x\n"); return 1; }
    int master = fd[0], slave = fd[1];
    g_master = master;

    struct winsize ws = { .ws_row = (unsigned short)ROWS, .ws_col = (unsigned short)COLS };
    ioctl(slave, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid == 0) {
        close(master); close(0); close(1);   // child: stdio is the slave tty
        setsid();
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) { close(slave); }
        // Make our own (new) process group the tty's FOREGROUND group BEFORE we
        // exec the shell, so busybox ash's job-control startup -- which spins in
        // `while (tcgetpgrp(tty) != getpgrp()) ...` until it is foreground --
        // sees a match immediately instead of hanging (there is no controlling-
        // terminal race because we set it here, in the child, pre-exec).
        tcsetpgrp(0, getpid());
        setenv("TERM", "xterm", 1);
        // busybox ash -- a real interactive shell with job control, line editing
        // and coloured `ls`. Works because the kernel's setsid() now moves the
        // child into its own process group, so ash's job-control startup (spin
        // until tcgetpgrp(tty) == getpgrp()) matches immediately. Falls back to
        // the native /bin/sh if busybox is missing.
        char *av[] = { "sh", NULL };
        execv("/bin/busybox", av);
        execv("/bin/sh", av);
        _exit(127);
    }
    close(slave);
    ioctl(master, TIOCSPGRP, &pid);

    vt = vterm_new(ROWS, COLS);
    vterm_set_utf8(vt, 1);
    vs = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(vs, &scr_cbs, NULL);
    vterm_screen_set_damage_merge(vs, VTERM_DAMAGE_ROW);
    vterm_screen_reset(vs, 1);
    vterm_output_set_callback(vt, out_cb, NULL);

    wrs("z "); wrint(ROWS); wrs(" "); wrint(COLS); wrs("\n");
    for (int r = 0; r < ROWS; r++) { dirty[r] = 1; }
    flush_damage();

    char lbuf[512]; int ll = 0;
    for (;;) {
        struct pollfd pf[2] = {
            { .fd = 0, .events = POLLIN },         // frame commands
            { .fd = master, .events = POLLIN },    // shell output
        };
        poll(pf, 2, -1);

        if (pf[1].revents & POLLIN) {
            char buf[4096];
            int n = (int)read(master, buf, sizeof(buf));
            if (n > 0) { vterm_input_write(vt, buf, (size_t)n); flush_damage(); }
            else if (n <= 0) { wrs("x\n"); break; }   // shell exited
        }
        if (pf[0].revents & POLLIN) {
            char buf[512];
            int n = (int)read(0, buf, sizeof(buf));
            if (n <= 0) { break; }                    // frame closed -> quit
            for (int i = 0; i < n; i++) {
                if (buf[i] == '\n') { lbuf[ll] = 0; handle_command(lbuf); ll = 0; }
                else if (ll < (int)sizeof(lbuf) - 1) { lbuf[ll++] = buf[i]; }
            }
        }
    }
    return 0;
}
