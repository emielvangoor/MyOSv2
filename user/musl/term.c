// term.c -- /bin/term: a standalone full-screen VT100 terminal on its own seat.
// ============================================================================
//
// MyOSv2's graphical frame is deliberately NOT a terminal -- it inserts program
// output into a Lisp buffer and strips every escape but SGR colour. Full-screen
// TUIs (vi, less, top) need the opposite: a real tty whose cursor-addressing
// escapes are interpreted into a 2-D screen. This program is that terminal.
//
// It is a separate process (like teapot), built against musl so libvterm -- the
// VT100/xterm screen model that Emacs's vterm and Neovim use -- links against a
// real libc. It owns a seat (its own virtual terminal; Ctrl-Alt-Fn switches to
// it), renders a grid of cells straight to the virtio-gpu framebuffer with the
// kernel's anti-aliased font, and runs `busybox sh` on a kernel pseudo-terminal:
//
//      keystrokes  --(evdev)-->  term  --(bytes)-->  pty master
//                                                       |  line discipline
//      framebuffer <--(blit)--  term  <--(bytes)--  pty master <-- sh writes
//
// The kernel PTY (src/pty.c) gives the shell a genuine tty: isatty() is true and
// tcsetattr() really switches raw/cooked, so vi can take over the screen.
//
// Phase 2 scope: bring up the pipeline end to end -- spawn the shell, render its
// screen, and feed it printable keys + Enter/Backspace/Tab. Full key encoding
// (arrows, function keys, modifiers, signal chars) is Phase 3.

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "vterm.h"
// The kernel's 20x40 anti-aliased glyphs. Included by direct relative path
// rather than -Isrc, because src/ also holds poll.h/string.h that would shadow
// musl's system headers. font_aa.h only needs <stdint.h>, so it is safe alone.
#include "../../src/font_aa.h"

// --- MyOSv2-native syscalls (graphics/input/pty) ----------------------------
// These live above the Linux range (MYOS_SYS_BASE = 0x1000) and have no musl
// wrapper, so we issue the raw `svc #0` ourselves. fork/exec/dup2/ioctl/poll/
// read/write come from musl (ordinary Linux syscalls the kernel already serves).
#define SYS_INPUT_READ  0x1006   // x0=struct input_event*, x1=nonblock -> 0/-1
#define SYS_GFX_ACQUIRE 0x1007   // x0=struct gfx_info* -> 0/-1 (also claims a seat)
#define SYS_GFX_FLUSH   0x1008   // x0=x,x1=y,x2=w,x3=h -> 0/-1 (active seat only)
#define SYS_OPENPT      0x1013   // x0=int fd[2] -> {master, slave}; 0/-1

struct gfx_info { void *fb; unsigned int w, h, pitch; };
struct input_event { uint16_t type, code; uint32_t value; };
#define EV_KEY 1

static long svc5(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}
static int gfx_acquire(struct gfx_info *g) { return (int)svc5(SYS_GFX_ACQUIRE, (long)g, 0, 0, 0, 0); }
static int gfx_flush(int x, int y, int w, int h) { return (int)svc5(SYS_GFX_FLUSH, x, y, w, h, 0); }
static int input_poll(struct input_event *e) { return (int)svc5(SYS_INPUT_READ, (long)e, 1, 0, 0, 0); }
static int openpt(int fd[2]) { return (int)svc5(SYS_OPENPT, (long)fd, 0, 0, 0, 0); }

// --- framebuffer + glyph blitting (copied math from rd_core.c) ---------------
#define CELL_W FONT_AA_W       // 20 px
#define CELL_H FONT_AA_H       // 40 px
static uint32_t *FB;           // 0x00RRGGBB framebuffer
static int STRIDE;             // pixels per row (pitch/4)
static unsigned SW, SH;        // screen pixel dimensions
static int COLS, ROWS;         // cell grid dimensions

// Integer alpha blend: out = bg + (fg-bg)*a/255. Same as rd_core's blend().
static inline uint32_t blend(uint32_t bg, uint32_t fg, uint32_t a)
{
    if (a == 0)   { return bg; }
    if (a == 255) { return fg; }
    int32_t ia = (int32_t)a;
    int32_t r = (int32_t)((bg >> 16) & 0xFF); r += (((int32_t)((fg >> 16) & 0xFF) - r) * ia) / 255;
    int32_t g = (int32_t)((bg >> 8)  & 0xFF); g += (((int32_t)((fg >> 8)  & 0xFF) - g) * ia) / 255;
    int32_t b = (int32_t)( bg        & 0xFF); b += (((int32_t)( fg        & 0xFF) - b) * ia) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Paint one cell: glyph `ch` in fg over bg, with optional bold/underline and
// inverse (used for the cursor and reverse-video cells). Non-ASCII -> '?'.
static void paint_cell(int col, int row, uint32_t ch, uint32_t fg, uint32_t bg,
                       int bold, int underline, int inverse)
{
    if (inverse) { uint32_t t = fg; fg = bg; bg = t; }
    int g = (ch >= FONT_AA_FIRST && ch <= FONT_AA_LAST) ? (int)ch : '?';
    const uint8_t *glyph = font_aa[g - FONT_AA_FIRST];
    for (int gy = 0; gy < CELL_H; gy++) {
        uint32_t *out = FB + (row * CELL_H + gy) * STRIDE + col * CELL_W;
        const uint8_t *arow = glyph + gy * CELL_W;
        for (int gx = 0; gx < CELL_W; gx++) {
            uint32_t a = arow[gx];
            if (bold && gx > 0 && arow[gx - 1] > a) { a = arow[gx - 1]; }  // fake bold
            out[gx] = blend(bg, fg, a);
        }
    }
    if (underline) {
        uint32_t *out = FB + (row * CELL_H + CELL_H - 2) * STRIDE + col * CELL_W;
        for (int gx = 0; gx < CELL_W; gx++) { out[gx] = fg; }
    }
}

// --- libvterm glue -----------------------------------------------------------
static VTerm *vt;
static VTermScreen *vs;
static int g_master;                       // pty master fd (libvterm replies go here)
static int cur_row, cur_col, cur_vis = 1;  // cursor position from libvterm

// libvterm wants to send bytes back to the program (replies to device queries,
// key encodings) -- forward them to the shell via the pty master.
static void out_cb(const char *s, size_t len, void *user)
{
    (void)user;
    if (len > 0) { (void)!write(g_master, s, len); }
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    (void)old; (void)user;
    cur_row = pos.row; cur_col = pos.col; cur_vis = visible;
    return 1;
}
static const VTermScreenCallbacks scr_cbs = { .movecursor = cb_movecursor };

// Repaint the whole grid from libvterm's screen and flush it. Simple and
// correct; damage-rect optimisation is deferred to a later phase.
static void render_all(void)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            VTermPos p = { .row = r, .col = c };
            VTermScreenCell cell;
            uint32_t ch = ' ', fgc = 0x00cccccc, bgc = 0x00000000;
            int bold = 0, ul = 0, inv = 0;
            if (vterm_screen_get_cell(vs, p, &cell)) {
                if (cell.chars[0]) { ch = cell.chars[0]; }
                VTermColor fg = cell.fg, bg = cell.bg;
                vterm_screen_convert_color_to_rgb(vs, &fg);
                vterm_screen_convert_color_to_rgb(vs, &bg);
                fgc = ((uint32_t)fg.rgb.red << 16) | ((uint32_t)fg.rgb.green << 8) | fg.rgb.blue;
                bgc = ((uint32_t)bg.rgb.red << 16) | ((uint32_t)bg.rgb.green << 8) | bg.rgb.blue;
                bold = cell.attrs.bold;
                ul = cell.attrs.underline != 0;
                inv = cell.attrs.reverse;
            }
            if (cur_vis && r == cur_row && c == cur_col) { inv = !inv; }  // block cursor
            paint_cell(c, r, ch, fgc, bgc, bold, ul, inv);
        }
    }
    gfx_flush(0, 0, (int)SW, (int)SH);
}

// --- keyboard: evdev keycode -> byte (Phase 2 minimal: US ASCII) -------------
// Indexed by Linux evdev KEY_* code. Phase 3 replaces this with the full
// libvterm key encoder (arrows, F-keys, modifiers).
static const char kc_lower[128] = {
    [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
    [12]='-',[13]='=',[16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',
    [23]='i',[24]='o',[25]='p',[26]='[',[27]=']',[30]='a',[31]='s',[32]='d',[33]='f',
    [34]='g',[35]='h',[36]='j',[37]='k',[38]='l',[39]=';',[40]='\'',[41]='`',[43]='\\',
    [44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',[51]=',',[52]='.',
    [53]='/',[57]=' ',
};
static const char kc_upper[128] = {
    [2]='!',[3]='@',[4]='#',[5]='$',[6]='%',[7]='^',[8]='&',[9]='*',[10]='(',[11]=')',
    [12]='_',[13]='+',[16]='Q',[17]='W',[18]='E',[19]='R',[20]='T',[21]='Y',[22]='U',
    [23]='I',[24]='O',[25]='P',[26]='{',[27]='}',[30]='A',[31]='S',[32]='D',[33]='F',
    [34]='G',[35]='H',[36]='J',[37]='K',[38]='L',[39]=':',[40]='"',[41]='~',[43]='|',
    [44]='Z',[45]='X',[46]='C',[47]='V',[48]='B',[49]='N',[50]='M',[51]='<',[52]='>',
    [53]='?',[57]=' ',
};

static int shift_down;   // tracks Left/Right Shift

// Translate one evdev key event into bytes written to the shell. Returns 0 on a
// key we handled (so the caller knows input happened).
static void handle_key(const struct input_event *ev)
{
    if (ev->type != EV_KEY) { return; }
    int code = ev->code, down = (ev->value != 0);   // 1=press, 2=repeat, 0=release
    if (code == 42 || code == 54) { shift_down = down; return; }   // L/R Shift
    if (!down) { return; }                                          // act on press only
    char b = 0;
    switch (code) {
    case 28: b = '\r'; break;     // Enter -> CR (ICRNL maps to NL for the shell)
    case 14: b = 0x7f; break;     // Backspace -> DEL (the VERASE default)
    case 15: b = '\t'; break;     // Tab
    case 1:  b = 0x1b; break;     // Esc
    default:
        if (code >= 0 && code < 128) { b = shift_down ? kc_upper[code] : kc_lower[code]; }
        break;
    }
    if (b) { (void)!write(g_master, &b, 1); }
}

int main(void)
{
    struct gfx_info gi;
    if (gfx_acquire(&gi) != 0) { return 1; }
    FB = (uint32_t *)gi.fb; SW = gi.w; SH = gi.h; STRIDE = (int)(gi.pitch / 4);
    COLS = (int)(gi.w / CELL_W); ROWS = (int)(gi.h / CELL_H);

    int fd[2];
    if (openpt(fd) != 0) { return 1; }
    int master = fd[0], slave = fd[1];
    g_master = master;

    // Tell the slave its window size before the shell starts, so child programs
    // that query TIOCGWINSZ see the real grid.
    struct winsize ws = { .ws_row = (unsigned short)ROWS, .ws_col = (unsigned short)COLS };
    ioctl(slave, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: become a session leader so the pty is our controlling terminal,
        // wire stdio to the slave, and exec the shell.
        close(master);
        setsid();
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) { close(slave); }
        setenv("TERM", "xterm", 1);
        char *argv[] = { "sh", NULL };
        execv("/bin/sh", argv);     // /bin/sh -> busybox sh applet
        _exit(127);
    }
    close(slave);
    ioctl(master, TIOCSPGRP, &pid);   // the child's group is the tty's foreground

    vt = vterm_new(ROWS, COLS);
    vterm_set_utf8(vt, 1);
    vs = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(vs, &scr_cbs, NULL);
    vterm_screen_reset(vs, 1);
    vterm_output_set_callback(vt, out_cb, NULL);
    // Default colours: light grey on black (the cell default before any SGR).
    VTermState *st = vterm_obtain_state(vt);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 0xcc, 0xcc, 0xcc);
    vterm_color_rgb(&bg, 0x00, 0x00, 0x00);
    vterm_state_set_default_colors(st, &fg, &bg);

    render_all();

    for (;;) {
        struct pollfd pf = { .fd = master, .events = POLLIN };
        poll(&pf, 1, 16);               // ~60 Hz cadence; also lets input drain
        int dirty = 0;
        if (pf.revents & POLLIN) {
            char buf[4096];
            int n = (int)read(master, buf, sizeof(buf));
            if (n > 0) { vterm_input_write(vt, buf, (size_t)n); dirty = 1; }
            else if (n == 0) { break; }  // shell exited -> master EOF
        }
        struct input_event ev;
        while (input_poll(&ev) == 0) { handle_key(&ev); }
        if (dirty) { render_all(); }
    }
    return 0;
}
