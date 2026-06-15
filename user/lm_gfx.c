/*
 * lm_gfx.c -- the graphical Lisp machine's display primitives (Phase 25.4).
 * ========================================================================
 *
 * This file fuses the two cores that /bin/lisp links: the LANGUAGE (lm_core)
 * and the VIEW (rd_core). Every DEFUN below lets Lisp mutate the redisplay
 * model -- buffers, the window tree, faces -- or pump the machinery
 * (redisplay, read-event). Lisp owns all semantics (what a key DOES lives in
 * frame.l); C owns the mechanics (what a key IS, where a glyph goes).
 *
 * User-build-only for the same reason as lm_sys.c: it talks to syscalls
 * (gfx_acquire, input_read) the kernel build of the cores must never see.
 */

#include "lm.h"
#include "rd.h"
#include "ulib.h"
#include "lm_sys.h"

/* ---- the machine's one frame + its buffer pool -------------------------- */

#define NBUFS 8
#define BUF_CAP 8192
#define SHOT_MAX_W 2560        /* widest scanout the screenshot row buffer holds;
                                  keep >= GFX_W in src/gfx.h */

static struct rd_frame frame;
static struct rd_cell front_grid[160 * 45], back_grid[160 * 45];
static struct rd_buffer bufs[NBUFS];
static char buf_store[NBUFS][BUF_CAP];
static int buf_used[NBUFS];

static int buf_shm[NBUFS];          /* shm handle per surface buffer (-1 = none) */
static struct gfx_info gi;          /* the mapped framebuffer */
static int frame_ready;

/* The selected window's buffer -- the one Lisp edits. */
static struct rd_buffer *cur(void) { return frame.selected->buf; }

/* ---- argument helpers (the lm_sys.c idiom) ------------------------------- */

static Lobj nth_arg(Lobj args, int n)
{
    while (n-- > 0) { args = CDR(args); }
    return CAR(args);
}
static int64_t req_fixnum(Lobj o, const char *who)
{
    if (!IS_FIXNUM(o)) { lm_error(who, o); }
    return FIXNUM_VAL(o);
}
static const char *req_string(Lobj o, const char *who)
{
    if (!IS_STRING(o)) { lm_error(who, o); }
    return ((LString *)PTR(o))->data;
}

#define DEFGFX(lisp_name, c_name, min, max)                                   \
    static Lobj c_name(Lobj args, Lobj env);                                  \
    static void register_##c_name(void) {                                     \
        Lobj sym = intern(lisp_name);                                         \
        ((Symbol *)PTR(sym))->function = make_prim(lisp_name, c_name, min, max); \
    }                                                                         \
    static Lobj c_name(Lobj args, Lobj env)

/* ---- frame bring-up ------------------------------------------------------- */

/* (frame-init) -> t/nil. Acquire the framebuffer and stand up the frame with
 * one buffer, "*repl*". Idempotent. */
DEFGFX("frame-init", Gframe_init, 0, 0) {
    (void)args; (void)env;
    if (frame_ready) { return Qt; }
    if (gfx_acquire(&gi) != 0) { return Qnil; }
    for (int i = 0; i < NBUFS; i++) { buf_used[i] = 0; }
    buf_used[0] = 1;
    rd_buf_init(&bufs[0], "*repl*", buf_store[0], BUF_CAP);
    rd_frame_init(&frame, (int)gi.w, (int)gi.h, front_grid, back_grid, &bufs[0]);
    frame_ready = 1;
    return Qt;
}

/* ---- buffers -------------------------------------------------------------- */

/* (make-buffer "name") -> handle (a fixnum index), or nil if the pool is full. */
DEFGFX("make-buffer", Gmake_buffer, 1, 1) {
    (void)env;
    const char *name = req_string(CAR(args), "make-buffer: name must be a string");
    for (int i = 0; i < NBUFS; i++) {
        if (!buf_used[i]) {
            buf_used[i] = 1;
            rd_buf_init(&bufs[i], name, buf_store[i], BUF_CAP);
            return FIXNUM(i);
        }
    }
    return Qnil;
}

/* (set-buffer handle) -> show that buffer in the selected window. */
DEFGFX("set-buffer", Gset_buffer, 1, 1) {
    (void)env;
    int i = (int)req_fixnum(CAR(args), "set-buffer: handle must be a fixnum");
    if (i < 0 || i >= NBUFS || !buf_used[i]) { lm_error("set-buffer: no such buffer", CAR(args)); }
    rd_set_buffer(&frame, &bufs[i]);
    return CAR(args);
}

/* (insert "str") -> insert at point in the selected window's buffer. */
DEFGFX("insert", Ginsert, 1, 1) {
    (void)env;
    rd_buf_insert(cur(), req_string(CAR(args), "insert: expected a string"));
    return Qt;
}

/* (delete-char n) -> delete n chars before point (backspace is n=1). */
DEFGFX("delete-char", Gdelete_char, 1, 1) {
    (void)env;
    rd_buf_delete(cur(), (int)req_fixnum(CAR(args), "delete-char: expected a fixnum"));
    return Qt;
}

DEFGFX("point", Gpoint, 0, 0) { (void)args; (void)env; return FIXNUM(cur()->point); }
DEFGFX("buffer-length", Gbuflen, 0, 0) { (void)args; (void)env; return FIXNUM(rd_buf_len(cur())); }

DEFGFX("goto-char", Ggoto_char, 1, 1) {
    (void)env;
    rd_buf_set_point(cur(), (int)req_fixnum(CAR(args), "goto-char: expected a fixnum"));
    return FIXNUM(cur()->point);
}

/* (buffer-substring start end) -> the text [start, end) as a string. The
 * REPL uses this to lift the line being edited out of the buffer. */
DEFGFX("buffer-substring", Gbufsub, 2, 2) {
    (void)env;
    int s = (int)req_fixnum(nth_arg(args, 0), "buffer-substring: start");
    int e = (int)req_fixnum(nth_arg(args, 1), "buffer-substring: end");
    static char tmp[1024];
    int n = 0;
    for (int p = s; p < e && n < (int)sizeof(tmp) - 1; p++) {
        int c = rd_buf_char_at(cur(), p);
        if (c < 0) { break; }
        tmp[n++] = (char)c;
    }
    tmp[n] = 0;
    return make_string(tmp);
}

/* ---- surface buffers: the EXWM move (Phase 25.6) -------------------------- */

/* (make-surface-buffer "name" w h) -> handle. The buffer's content is a PIXEL
 * CANVAS in shared memory instead of text: in-image Lisp draws into it with
 * surface-fill-rect, or an external program maps the same shm and renders
 * (run-in-buffer). rd_core blits it into the window each redisplay. */
DEFGFX("make-surface-buffer", Gmake_surface, 3, 3) {
    (void)env;
    const char *name = req_string(nth_arg(args, 0), "make-surface-buffer: name");
    int w = (int)req_fixnum(nth_arg(args, 1), "make-surface-buffer: w");
    int h = (int)req_fixnum(nth_arg(args, 2), "make-surface-buffer: h");
    if (w < 1 || h < 1 || w > (int)gi.w || h > (int)gi.h) {
        lm_error("make-surface-buffer: bad size", nth_arg(args, 1));
    }
    for (int i = 0; i < NBUFS; i++) {
        if (!buf_used[i]) {
            int handle = shm_create((unsigned long)w * h * 4);
            if (handle < 0) { return Qnil; }
            void *cv = shm_map(handle);
            if (!cv) { return Qnil; }
            buf_used[i] = 1;
            rd_buf_init(&bufs[i], name, buf_store[i], BUF_CAP);
            bufs[i].kind = RD_SURFACE;
            bufs[i].canvas = cv; bufs[i].cv_w = w; bufs[i].cv_h = h;
            buf_shm[i] = handle;
            return FIXNUM(i);
        }
    }
    return Qnil;
}

/* (surface-fill-rect buf x y w h color) -- draw from Lisp (color 0x00RRGGBB). */
DEFGFX("surface-fill-rect", Gsurf_fill, 6, 6) {
    (void)env;
    int i = (int)req_fixnum(nth_arg(args, 0), "surface-fill-rect: buffer");
    if (i < 0 || i >= NBUFS || !buf_used[i] || bufs[i].kind != RD_SURFACE) {
        lm_error("surface-fill-rect: not a surface buffer", nth_arg(args, 0));
    }
    struct rd_buffer *b = &bufs[i];
    int x = (int)req_fixnum(nth_arg(args, 1), "x"), y = (int)req_fixnum(nth_arg(args, 2), "y");
    int w = (int)req_fixnum(nth_arg(args, 3), "w"), h = (int)req_fixnum(nth_arg(args, 4), "h");
    uint32_t c = (uint32_t)req_fixnum(nth_arg(args, 5), "color");
    for (int yy = y; yy < y + h && yy < b->cv_h; yy++) {
        if (yy < 0) { continue; }
        for (int xx = x; xx < x + w && xx < b->cv_w; xx++) {
            if (xx >= 0) { b->canvas[yy * b->cv_w + xx] = c; }
        }
    }
    return Qt;
}

/* (run-in-buffer buf "prog") -> pid: fork+exec /bin/<prog> handing it the
 * canvas as argv: <prog> <shm-handle> <w> <h>. The program shm_map()s the
 * handle and draws; its pixels appear in the buffer's window. A tiny Wayland
 * client's contract, but the "window" is an Emacs buffer. */
static void itoa10(long v, char *out)
{
    char tmp[16]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (n > 0) { out[i++] = tmp[--n]; }
    out[i] = 0;
}
DEFGFX("run-in-buffer", Grun_in_buffer, 2, 2) {
    (void)env;
    int i = (int)req_fixnum(nth_arg(args, 0), "run-in-buffer: buffer");
    const char *prog = req_string(nth_arg(args, 1), "run-in-buffer: program");
    if (i < 0 || i >= NBUFS || !buf_used[i] || bufs[i].kind != RD_SURFACE) {
        lm_error("run-in-buffer: not a surface buffer", nth_arg(args, 0));
    }
    char path[64], hs[16], ws[16], hh[16];
    int n = 0;
    const char *pre = "/bin/";
    while (pre[n]) { path[n] = pre[n]; n++; }
    for (int j = 0; prog[j] && n < 62; j++) { path[n++] = prog[j]; }
    path[n] = 0;
    itoa10(buf_shm[i], hs); itoa10(bufs[i].cv_w, ws); itoa10(bufs[i].cv_h, hh);
    long pid = sys_fork();
    if (pid == 0) {
        char *argv[5] = { (char *)prog, hs, ws, hh, 0 };
        sys_exec(path, argv);
        sys_exit(127);
    }
    return FIXNUM(pid);
}

/* ---- windows + faces ------------------------------------------------------ */

DEFGFX("split-below", Gsplit_below, 0, 0) { (void)args; (void)env; return rd_split(&frame, 0) ? Qt : Qnil; }
DEFGFX("split-right", Gsplit_right, 0, 0) { (void)args; (void)env; return rd_split(&frame, 1) ? Qt : Qnil; }
DEFGFX("other-window", Gother_window, 0, 0) { (void)args; (void)env; rd_other_window(&frame); return Qt; }
DEFGFX("delete-window", Gdelete_window, 0, 0) { (void)args; (void)env; return rd_win_delete(&frame) == 0 ? Qt : Qnil; }

/* (set-face id fg bg) -- colors as 0x00RRGGBB fixnums. */
DEFGFX("set-face", Gset_face, 3, 3) {
    (void)env;
    int id = (int)req_fixnum(nth_arg(args, 0), "set-face: id");
    if (id < 0 || id >= RD_NFACES) { lm_error("set-face: bad id", nth_arg(args, 0)); }
    frame.faces[id].fg = (uint32_t)req_fixnum(nth_arg(args, 1), "set-face: fg");
    frame.faces[id].bg = (uint32_t)req_fixnum(nth_arg(args, 2), "set-face: bg");
    return Qt;
}

/* (echo "msg") -> the echo area (the frame's last line). */
DEFGFX("echo", Gecho, 1, 1) {
    (void)env;
    rd_echo(&frame, req_string(CAR(args), "echo: expected a string"));
    return Qt;
}

/* (select-window-at x y) -- mouse click routing: select the leaf window whose
 * rect contains the pixel. The tree walk is C because the tree is C. */
static struct rd_win *leaf_at(struct rd_win *w, int col, int row)
{
    if (w->leaf) { return w; }
    struct rd_win *a = w->a, *b = w->b;
    if (w->vertical) {
        return (col < b->x) ? leaf_at(a, col, row) : leaf_at(b, col, row);
    }
    return (row < b->y) ? leaf_at(a, col, row) : leaf_at(b, col, row);
}
DEFGFX("select-window-at", Gselect_at, 2, 2) {
    (void)env;
    int px = (int)req_fixnum(nth_arg(args, 0), "select-window-at: x");
    int py = (int)req_fixnum(nth_arg(args, 1), "select-window-at: y");
    frame.selected = leaf_at(frame.root, px / RD_CELL_W, py / RD_CELL_H);
    return Qt;
}

/* ---- redisplay ------------------------------------------------------------ */

/* (redisplay) -> make the screen match the model: rd_redisplay paints changed
 * cells into the mapped framebuffer, then each damage rect is gfx_flush'd. */
DEFGFX("redisplay", Gredisplay, 0, 0) {
    (void)args; (void)env;
    if (!frame_ready) { return Qnil; }
    struct rd_rect rects[RD_MAX_RECTS];
    int n = rd_redisplay(&frame, (uint32_t *)gi.fb, (int)(gi.pitch / 4),
                         rects, RD_MAX_RECTS);
    for (int i = 0; i < n; i++) {
        gfx_flush(rects[i].x, rects[i].y, rects[i].w, rects[i].h);
    }
    return FIXNUM(n);
}

/* ---- input: raw evdev -> cooked Lisp events ------------------------------- */
/*
 * The kernel hands us evdev triples; Lisp wants meaning. The cooking -- what
 * a keycode IS (scancode 30 = 'a', shift makes it 'A') -- is mechanical and
 * lives here; what a key DOES is policy and lives in frame.l. Cooked shapes:
 *   (char N)      printable ascii, shift applied; also 10=RET 8=BKSP 9=TAB
 *   (ctrl N)      Ctrl+letter, N = the lowercase ascii
 *   (mouse X Y)   left-button press, in pixels
 */
static const char keymap_lo[] =
    "\0\033" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char keymap_hi[] =
    "\0\033" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static int ck_shift, ck_ctrl, ck_alt;
static int ck_mx, ck_my;            /* last absolute pointer position, pixels */

/* Cook one raw event; returns the Lisp event, or 0 for "nothing yet". */
static Lobj cook_event(struct input_event *evp)
{
    struct input_event ev = *evp;
    {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) { ck_mx = (int)((uint64_t)ev.value * gi.w / 32768); }
            if (ev.code == ABS_Y) { ck_my = (int)((uint64_t)ev.value * gi.h / 32768); }
            return 0;
        }
        if (ev.type != EV_KEY) { return 0; }
        if (ev.code == 42 || ev.code == 54) {            /* shift down/up */
            ck_shift = (ev.value != 0); return 0;
        }
        if (ev.code == 29 || ev.code == 97) {            /* ctrl down/up */
            ck_ctrl = (ev.value != 0); return 0;
        }
        if (ev.code == 56 || ev.code == 100) {           /* alt (Meta) down/up */
            ck_alt = (ev.value != 0); return 0;
        }
        if (ev.value == 1 && (ev.code == 103 || ev.code == 108)) {
            /* arrows: cook Up/Down into C-p / C-n -- vertico's navigation. */
            return make_cons(intern("ctrl"),
                   make_cons(FIXNUM(ev.code == 103 ? 'p' : 'n'), Qnil));
        }
        if (ev.code == 272) {                            /* BTN_LEFT */
            if (ev.value == 1) {
                return make_cons(intern("mouse"),
                       make_cons(FIXNUM(ck_mx), make_cons(FIXNUM(ck_my), Qnil)));
            }
            return 0;
        }
        if (ev.value != 1) { return 0; }                 /* key releases */
        if (ev.code < sizeof(keymap_lo)) {
            char c = (ck_shift ? keymap_hi : keymap_lo)[ev.code];
            if (c) {
                const char *tag = ck_ctrl ? "ctrl" : (ck_alt ? "meta" : "char");
                int ch = (ck_ctrl || ck_alt) ? (c | 0x20) : c; /* modifiers: lowercase */
                return make_cons(intern(tag), make_cons(FIXNUM(ch), Qnil));
            }
        }
        return 0;
    }
}

DEFGFX("read-event", Gread_event, 0, 0) {
    (void)args; (void)env;
    struct input_event ev;
    for (;;) {
        if (input_read(&ev) != 0) { return Qnil; }      /* EINTR */
        Lobj cooked = cook_event(&ev);
        if (cooked) { return cooked; }
    }
}

/* (read-event-nowait) -> a cooked event, or nil when nothing is pending.
 * What lets a streaming loop (run's output pump) stay responsive to C-c. */
DEFGFX("read-event-nowait", Gread_event_nb, 0, 0) {
    (void)args; (void)env;
    struct input_event ev;
    while (input_poll(&ev) == 0) {
        Lobj cooked = cook_event(&ev);
        if (cooked) { return cooked; }
    }
    return Qnil;
}

/* (poll-fd fd ms) -> t when fd is readable within ms, else nil. */
DEFGFX("poll-fd", Gpoll_fd, 2, 2) {
    (void)env;
    struct pollfd p;
    p.fd = (int)req_fixnum(nth_arg(args, 0), "poll-fd: fd");
    p.events = POLLIN; p.revents = 0;
    int n = poll(&p, 1, (int)req_fixnum(nth_arg(args, 1), "poll-fd: ms"));
    return (n > 0 && (p.revents & POLLIN)) ? Qt : Qnil;
}

/* ---- introspection: the image examining itself (M-x, describe) ------------ */

/* (function-info 'sym) -> what lives in the symbol's function slot:
 *   nil                          not a function
 *   (lambda PARAMS BODY)         a defun -- BODY is the LIVE source
 *   (macro PARAMS BODY)          a defmacro
 *   (primitive "name" MIN MAX)   a C primitive
 * describe-function on a Lisp machine doesn't read documentation ABOUT the
 * system; it shows the living object itself. */
DEFGFX("function-info", Gfunction_info, 1, 1) {
    (void)env;
    Lobj sym = CAR(args);
    if (!IS_SYMBOL(sym)) { lm_error("function-info: expected a symbol", sym); }
    Lobj fn = ((Symbol *)PTR(sym))->function;
    if (IS_PRIM(fn)) {
        Prim *p = PTR(fn);
        return make_cons(intern("primitive"),
               make_cons(make_string(p->name),
               make_cons(FIXNUM(p->min_args),
               make_cons(FIXNUM(p->max_args), Qnil))));
    }
    if (IS_LAMBDA(fn)) {
        Cons *lc = PTR(fn);
        return make_cons(intern("lambda"),
               make_cons(lc->car, make_cons(CAR(lc->cdr), Qnil)));
    }
    if (IS_CONS(fn) && CAR(fn) == intern("macro") && IS_LAMBDA(CDR(fn))) {
        Cons *mc = PTR(CDR(fn));
        return make_cons(intern("macro"),
               make_cons(mc->car, make_cons(CAR(mc->cdr), Qnil)));
    }
    return Qnil;
}

/* (all-symbols) -> every interned symbol, as a list. The symbol table IS the
 * command registry -- this is what M-x completes over. */
DEFGFX("all-symbols", Gall_symbols, 0, 0) {
    (void)args; (void)env;
    Lobj out = Qnil;
    for (int i = symtab_count - 1; i >= 0; i--) {
        out = make_cons(symtab[i], out);
    }
    return out;
}

/* (string-search needle hay) -> index of the first match, or nil. */
DEFGFX("string-search", Gstring_search, 2, 2) {
    (void)env;
    const char *needle = req_string(nth_arg(args, 0), "string-search: needle");
    const char *hay = req_string(nth_arg(args, 1), "string-search: haystack");
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) { j++; }
        if (!needle[j]) { return FIXNUM(i); }
    }
    return Qnil;
}

/* (substring str start end) -> the slice [start, end). */
DEFGFX("substring", Gsubstring, 3, 3) {
    (void)env;
    Lobj str = nth_arg(args, 0);
    if (!IS_STRING(str)) { lm_error("substring: expected a string", str); }
    LString *ls = (LString *)PTR(str);
    int a = (int)req_fixnum(nth_arg(args, 1), "substring: start");
    int b = (int)req_fixnum(nth_arg(args, 2), "substring: end");
    if (a < 0) { a = 0; }
    if (b > (int)ls->len) { b = (int)ls->len; }
    static char tmp[512];
    int n = 0;
    for (int i = a; i < b && n < (int)sizeof(tmp) - 1; i++) { tmp[n++] = ls->data[i]; }
    tmp[n] = 0;
    return make_string(tmp);
}

/* (echo-select n) -> highlight echo line n as the vertico selection bar. */
DEFGFX("echo-select", Gecho_select, 1, 1) {
    (void)env;
    rd_echo_select(&frame, (int)req_fixnum(CAR(args), "echo-select: expected a fixnum"));
    return Qt;
}

/* ---- strings <-> forms (the REPL's plumbing) ------------------------------- */

/* (read-string "src") -> the first form in the string, as data. */
DEFGFX("read-string", Gread_string, 1, 1) {
    (void)env;
    const char *src = req_string(CAR(args), "read-string: expected a string");
    Reader r;
    reader_from_string(&r, src, (long)((LString *)PTR(CAR(args)))->len);
    return lm_read(&r);
}

/* (string-from-char N) -> a one-character string: how self-insert turns a
 * cooked (char N) event back into buffer text. */
DEFGFX("string-from-char", Gstr_from_char, 1, 1) {
    (void)env;
    char tmp[2];
    tmp[0] = (char)req_fixnum(CAR(args), "string-from-char: expected a fixnum");
    tmp[1] = 0;
    return make_string(tmp);
}

/* (prin1-to-string obj) -> obj printed into a string. */
DEFGFX("prin1-to-string", Gprin1str, 1, 1) {
    (void)env;
    static char tmp[1024];
    lm_print_cstr(CAR(args), tmp, sizeof(tmp));
    return make_string(tmp);
}

/* ---- the machine photographs itself ---------------------------------------- */

/* (screenshot "/disk/shot.ppm") -> dump the framebuffer as a binary PPM (P6)
 * to a file. The fb is just process memory and /disk is the persistent SFS,
 * so the OS can keep evidence of what its screen looked like. */
DEFGFX("screenshot", Gscreenshot, 1, 1) {
    (void)env;
    const char *path = req_string(CAR(args), "screenshot: expected a path");
    if (!frame_ready) { return Qnil; }
    long fd = sys_creat(path);          /* open, creating it if missing */
    if (fd < 0) { return Qnil; }
    /* Build the header from the ACTUAL framebuffer size, not a constant: the
     * resolution is a compile-time knob (2x for HiDPI), and a header that
     * disagrees with the pixels writes a corrupt PPM. */
    char hdr[40], num[16];
    int n = 0, k;
    hdr[n++] = 'P'; hdr[n++] = '6'; hdr[n++] = '\n';
    itoa10((long)gi.w, num); for (k = 0; num[k]; k++) { hdr[n++] = num[k]; }
    hdr[n++] = ' ';
    itoa10((long)gi.h, num); for (k = 0; num[k]; k++) { hdr[n++] = num[k]; }
    hdr[n++] = '\n';
    hdr[n++] = '2'; hdr[n++] = '5'; hdr[n++] = '5'; hdr[n++] = '\n';
    sys_write((int)fd, hdr, n);
    /* One pixel row at a time: 0x00RRGGBB words -> R,G,B bytes. The row buffer
     * must hold the WIDEST scanout (SHOT_MAX_W tracks GFX_W in src/gfx.h);
     * guard rather than overflow if a future bump outgrows it. */
    static unsigned char row[SHOT_MAX_W * 3];
    if (gi.w > SHOT_MAX_W) { sys_close((int)fd); return Qnil; }
    uint32_t *fb = gi.fb;
    for (unsigned y = 0; y < gi.h; y++) {
        for (unsigned x = 0; x < gi.w; x++) {
            uint32_t px = fb[y * (gi.pitch / 4) + x];
            row[x * 3 + 0] = (px >> 16) & 0xFF;
            row[x * 3 + 1] = (px >> 8) & 0xFF;
            row[x * 3 + 2] = px & 0xFF;
        }
        if (sys_write((int)fd, row, (long)(gi.w * 3)) < 0) {
            sys_close((int)fd);          /* out of space: say so, don't lie */
            return Qnil;
        }
    }
    sys_close((int)fd);
    return Qt;
}

/* ---- routing the image's output into the frame ----------------------------- */
/*
 * In -frame mode, lm_cur_out points at this capture buffer for every tick of
 * the event loop. Whatever the image prints -- (princ ...), and crucially
 * lm_error's "ERROR: ..." lines -- accumulates here, and frame.l drains it
 * into the current buffer with (frame-output). Errors become visible in the
 * frame instead of leaking to the serial console.
 */
static char   tick_out[1024];
static Writer tick_writer;

Writer *lm_gfx_tick_writer(void)
{
    /* Initialize ONCE: the C loop fetches this every tick, and re-initializing
     * would wipe the text an aborted tick left behind -- exactly the ERROR
     * line the next tick must show. (frame-output) does the draining. */
    static int inited;
    if (!inited) {
        writer_to_buffer(&tick_writer, tick_out, sizeof(tick_out));
        inited = 1;
    }
    return &tick_writer;
}

/* (frame-output) -> everything printed since the last drain, as a string. */
DEFGFX("frame-output", Gframe_output, 0, 0) {
    (void)args; (void)env;
    Lobj s = make_string(tick_out);
    tick_out[0] = 0;
    tick_writer.len = 0;
    return s;
}

/* ---- registration ----------------------------------------------------------- */

void lm_gfx_register(void)
{
    register_Gframe_init();
    register_Gmake_buffer(); register_Gset_buffer();
    register_Ginsert(); register_Gdelete_char();
    register_Gpoint(); register_Gbuflen(); register_Ggoto_char(); register_Gbufsub();
    register_Gsplit_below(); register_Gsplit_right();
    register_Gother_window(); register_Gdelete_window();
    register_Gset_face(); register_Gecho(); register_Gselect_at();
    register_Gredisplay(); register_Gread_event();
    register_Gread_event_nb(); register_Gpoll_fd();
    register_Gframe_output();
    register_Gread_string(); register_Gprin1str(); register_Gstr_from_char();
    register_Gmake_surface(); register_Gsurf_fill(); register_Grun_in_buffer();
    register_Gfunction_info(); register_Gall_symbols(); register_Gstring_search();
    register_Gsubstring(); register_Gecho_select();
    register_Gscreenshot();
}
