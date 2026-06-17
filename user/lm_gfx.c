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

/* True once this process has acquired the framebuffer (it IS the graphical
 * frame). Exposed so (exit) can refuse: exiting the frame process freezes the
 * display, which looks like a machine lock. */
int gfx_frame_ready(void) { return frame_ready; }

/* ---- face name registry -------------------------------------------------- */
/*
 * face_names[id] is the interned symbol whose name was passed to (defface).
 * Slot 0 = "default", 1 = "mode-line", 2 = "region" (registered at gfx init).
 * Remaining slots are filled on demand by defface / face_alloc.
 *
 * WHY interned symbols are GC-safe here:
 *   intern() adds the symbol to symtab[], and gc_collect's mark phase starts
 *   by marking every entry in symtab[].  So any symbol returned by intern()
 *   is a GC root and will never be swept.  Storing it in face_names[] is
 *   therefore safe without a separate root -- but we ALSO mark it in
 *   gfx_gc_mark_buffers() as an extra safety net (cheap, belt-and-suspenders).
 */
static Lobj face_names[RD_NFACES];

/* Find an existing face id by name, or claim the next free slot.
 * Returns -1 if the table is full.  The SINGLE place that increments
 * frame.n_faces_used so defface and rd_resolve_face never collide. */
static int face_alloc(Lobj name)
{
    int i;
    /* Re-use an existing slot for the same name (idempotent defface). */
    for (i = 0; i < frame.n_faces_used; i++) {
        if (face_names[i] == name) { return i; }
    }
    if (frame.n_faces_used >= RD_NFACES) { return -1; }
    i = frame.n_faces_used++;
    face_names[i] = name;
    return i;
}

/* Look up a face id by its interned name symbol.
 * Returns 0 (default) for unknown names so callers always get a safe value.
 * Declared in rd.h (LM_BUILD section) so rd_resolve_face can call it. */
int gfx_face_id(Lobj name)
{
    for (int i = 0; i < frame.n_faces_used; i++) {
        if (face_names[i] == name) { return i; }
    }
    return 0;
}

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
 * one buffer, "*repl*". Idempotent.
 *
 * Face-name bootstrap: rd_frame_init sets n_faces_used=3 for the three
 * built-in slots 0/1/2.  We register their interned names here so that
 * (face-id 'default) -> 0, 'mode-line -> 1, 'region -> 2, before any
 * user (defface ...) call runs.  We use face_names[] directly (bypassing
 * face_alloc) because the ids are ALREADY claimed by rd_frame_init and we
 * must not double-increment n_faces_used. */
DEFGFX("frame-init", Gframe_init, 0, 0) {
    (void)args; (void)env;
    if (frame_ready) { return Qt; }
    if (gfx_acquire(&gi) != 0) { return Qnil; }
    for (int i = 0; i < NBUFS; i++) { buf_used[i] = 0; }
    buf_used[0] = 1;
    rd_buf_init(&bufs[0], "*repl*", buf_store[0], BUF_CAP);
    rd_frame_init(&frame, (int)gi.w, (int)gi.h, front_grid, back_grid, &bufs[0]);
    /* Zero out name slots so gfx_face_id's loop never reads uninitialised Lobj
     * values (Qnil compares unequal to any real symbol). */
    for (int i = 0; i < RD_NFACES; i++) { face_names[i] = Qnil; }
    /* Bind the three built-in face ids to their canonical names. */
    face_names[0] = intern("default");
    face_names[1] = intern("mode-line");
    face_names[2] = intern("region");
    /* n_faces_used is already 3 from rd_frame_init -- do NOT touch it here. */
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

/* (current-buffer) -> the handle of the SELECTED window's buffer. Lisp needs
 * this to tell the REPL apart from an editable file buffer: the same keystroke
 * means "evaluate" in one and "insert a newline" in the other. */
DEFGFX("current-buffer", Gcurrent_buffer, 0, 0) {
    (void)args; (void)env;
    return FIXNUM((int)(frame.selected->buf - bufs));
}

/* (kill-buffer) -> close the SELECTED window's buffer: retarget every window
 * showing it to another live buffer, then free its slot. Returns the new
 * buffer's handle, or nil if it is the last buffer (we never kill the last one).
 * This is how a REPL -- a "process in a buffer" -- ends: (exit) typed in a repl
 * buffer calls this to close it, returning to whatever buffer remains. */
DEFGFX("kill-buffer", Gkill_buffer, 0, 0) {
    (void)args; (void)env;
    int i = (int)(frame.selected->buf - bufs);
    if (i < 0 || i >= NBUFS || !buf_used[i]) { return Qnil; }
    // Choose where to land: prefer a buffer that is NOT a "*repl*" (so closing a
    // REPL returns you to *scratch* or a file, not a look-alike REPL); fall back
    // to any other live buffer.
    int used = 0, other = -1, nonrepl = -1;
    for (int k = 0; k < NBUFS; k++) {
        if (!buf_used[k]) { continue; }
        used++;
        if (k == i) { continue; }
        if (other < 0) { other = k; }
        { const char *nm = bufs[k].name; const char *r = "*repl*"; int e = 1;
          while (*nm && *nm == *r) { nm++; r++; } if (*nm != *r) { e = 0; }
          if (nonrepl < 0 && !e) { nonrepl = k; } }
    }
    if (nonrepl >= 0) { other = nonrepl; }
    if (used <= 1 || other < 0) { return Qnil; }     // keep at least one buffer alive
    // Retarget every window (selected or not) that shows the dying buffer, so no
    // window is left pointing at a freed slot.
    for (int wi = 0; wi < RD_MAX_WIN; wi++) {
        if (frame.wins[wi].used && frame.wins[wi].leaf && frame.wins[wi].buf == &bufs[i]) {
            frame.wins[wi].buf = &bufs[other];
            frame.wins[wi].top_line = 0;
        }
    }
    buf_used[i] = 0;
    buf_shm[i] = -1;            // a surface's shm object is left to the kernel
    return FIXNUM(other);
}

/* (set-mode-line-name "str") -> set the SELECTED window's buffer's mode-line
 * name (the "(Mode)" shown in the mode line). Lisp's set-major-mode calls it. */
DEFGFX("set-mode-line-name", Gset_mode_line_name, 1, 1) {
    (void)env;
    const char *s = req_string(CAR(args), "set-mode-line-name: expected a string");
    struct rd_buffer *b = cur();
    int i = 0;
    for (; s[i] && i < (int)sizeof(b->mode_line) - 1; i++) { b->mode_line[i] = s[i]; }
    b->mode_line[i] = 0;
    return Qt;
}

/* (set-line-wrap n) -> set the SELECTED window's buffer's line-wrap minor
 * mode. 0 = truncate long lines (default), non-0 = wrap onto the next row. */
DEFGFX("set-line-wrap", Gset_line_wrap, 1, 1) {
    (void)env;
    cur()->wrap = req_fixnum(CAR(args), "set-line-wrap: expected a fixnum") ? 1 : 0;
    return Qt;
}

/* A propertized string (Emacs `propertize`) is represented as the 3-list
 *   (propertized-string RAW PLIST)
 * where RAW is the plain string and PLIST is a uniform property list applied to
 * the whole string. `insert` recognizes it and stamps the properties onto the
 * inserted range; everything else inserts as plain text. */
static Lobj sym_propstr(void) { return intern("propertized-string"); }
static int is_propstr(Lobj x) { return IS_CONS(x) && CAR(x) == sym_propstr(); }

/* (propertize STRING PROP VAL ...) -> a propertized string carrying the props.
 * The rest args are ALREADY in plist shape (PROP VAL PROP VAL ...), so we wrap
 * them verbatim. */
DEFGFX("propertize", Gpropertize, 1, 64) {
    (void)env;
    Lobj str = CAR(args);
    if (!IS_STRING(str)) { lm_error("propertize: first arg must be a string", str); }
    Lobj plist = CDR(args);                         // (PROP VAL PROP VAL ...) == a plist
    return make_cons(sym_propstr(), make_cons(str, make_cons(plist, Qnil)));
}

/* (insert X) -> insert at point in the selected window's buffer. X is a plain
 * string, OR a propertized string (then its properties are applied to the range
 * just inserted -- this is how `ansi-color-apply` lands colored text). */
DEFGFX("insert", Ginsert, 1, 1) {
    (void)env;
    Lobj x = CAR(args);
    if (is_propstr(x)) {
        Lobj raw   = CAR(CDR(x));                    // (propertized-string RAW PLIST)
        Lobj plist = CAR(CDR(CDR(x)));
        int start = cur()->point;
        rd_buf_insert(cur(), req_string(raw, "insert: propertized raw must be a string"));
        int end = cur()->point;
        rd_set_text_props(cur(), start, end, plist);
        return Qt;
    }
    rd_buf_insert(cur(), req_string(x, "insert: expected a string"));
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

/* (char-at pos) -> the character code at POS, or -1 past the end. The eyes of
 * the editing commands: line/word motion in frame.l scans the buffer with it. */
DEFGFX("char-at", Gchar_at, 1, 1) {
    (void)env;
    return FIXNUM(rd_buf_char_at(cur(), (int)req_fixnum(CAR(args), "char-at: pos")));
}

/* (string-ref STR I) -> the byte at index I of STR (a fixnum 0..255), or -1 past
 * the end. char-at reads the buffer; this reads an arbitrary string -- needed by
 * ansi-color-apply to scan a program's output chunk for ESC/CSI bytes and digits. */
DEFGFX("string-ref", Gstring_ref, 2, 2) {
    (void)env;
    const char *s = req_string(CAR(args), "string-ref: expected a string");
    int i = (int)req_fixnum(nth_arg(args, 1), "string-ref: index");
    int n = 0; while (s[n]) { n++; }
    if (i < 0 || i >= n) { return FIXNUM(-1); }
    return FIXNUM((unsigned char)s[i]);
}

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
DEFGFX("delete-other-windows", Gdelete_other, 0, 0) { (void)args; (void)env; rd_delete_other(&frame); return Qt; }

/* (buffer-list) -> a list of (handle . name) for every live buffer. The data
 * behind C-x C-b and any future switch-to-buffer; built newest-handle-last. */
DEFGFX("buffer-list", Gbuffer_list, 0, 0) {
    (void)args; (void)env;
    Lobj out = Qnil;
    for (int i = NBUFS - 1; i >= 0; i--) {
        if (buf_used[i]) {
            out = make_cons(make_cons(FIXNUM(i), make_string(bufs[i].name)), out);
        }
    }
    return out;
}

/* (set-face id fg bg) -- legacy numeric face setter (colors as 0x00RRGGBB).
 * The numeric `id` path bypasses the name registry; it is kept for backward
 * compatibility with old Lisp code that drives faces by index directly. */
DEFGFX("set-face", Gset_face, 3, 3) {
    (void)env;
    int id = (int)req_fixnum(nth_arg(args, 0), "set-face: id");
    if (id < 0 || id >= RD_NFACES) { lm_error("set-face: bad id", nth_arg(args, 0)); }
    frame.faces[id].fg = (uint32_t)req_fixnum(nth_arg(args, 1), "set-face: fg");
    frame.faces[id].bg = (uint32_t)req_fixnum(nth_arg(args, 2), "set-face: bg");
    /* Mark both colors as set so the numeric path integrates cleanly with the
     * merge resolver (a face with fg_set=0 would inherit from default). */
    frame.faces[id].fg_set = 1;
    frame.faces[id].bg_set = 1;
    return Qt;
}

/* ---- keyword attribute parser (shared by defface and set-face-attribute) --
 *
 * Walk `rest` two at a time as (:kw val :kw val ...).  For each pair, update
 * the face slot at `id` in frame.faces[].  Recognized keywords:
 *   :foreground  0x00RRGGBB fixnum  -> fg + fg_set=1
 *   :background  0x00RRGGBB fixnum  -> bg + bg_set=1
 *   :bold        non-nil/nil        -> bold bit
 *   :inverse     non-nil/nil        -> inverse bit (swaps fg/bg at paint)
 *   :underline   non-nil/nil        -> underline bit
 * Unknown keywords are silently skipped (future-proof). */
static void face_set_kw(int id, Lobj rest)
{
    static const char kfg[]  = ":foreground";
    static const char kbg[]  = ":background";
    static const char kbold[] = ":bold";
    static const char kinv[]  = ":inverse";
    static const char kul[]   = ":underline";
    /* Intern once -- intern() returns the canonical symbol so == comparisons
     * work without a string compare on every pair. */
    Lobj sfg  = intern(kfg);
    Lobj sbg  = intern(kbg);
    Lobj sbold = intern(kbold);
    Lobj sinv  = intern(kinv);
    Lobj sul   = intern(kul);
    for (Lobj p = rest; IS_CONS(p) && IS_CONS(CDR(p)); p = CDR(CDR(p))) {
        Lobj kw  = CAR(p);
        Lobj val = CAR(CDR(p));
        if (kw == sfg) {
            frame.faces[id].fg     = (uint32_t)req_fixnum(val, "face: :foreground must be fixnum");
            frame.faces[id].fg_set = 1;
        } else if (kw == sbg) {
            frame.faces[id].bg     = (uint32_t)req_fixnum(val, "face: :background must be fixnum");
            frame.faces[id].bg_set = 1;
        } else if (kw == sbold) {
            frame.faces[id].bold   = IS_NIL(val) ? 0 : 1;
        } else if (kw == sinv) {
            frame.faces[id].inverse = IS_NIL(val) ? 0 : 1;
        } else if (kw == sul) {
            frame.faces[id].underline = IS_NIL(val) ? 0 : 1;
        }
        /* unknown keyword -> skip the pair */
    }
}

/* (defface NAME :kw val ...) -> face id (a fixnum).
 * Register or update a named face.  NAME must be a symbol.  Keyword/value
 * pairs following the name set the face's attributes (see face_set_kw).
 * If NAME was already registered its existing slot is updated in place;
 * otherwise the next free slot is claimed (n_faces_used advances).
 *
 * Example: (defface 'ansi-red :foreground 0xCC2424 :background 0x1D2021) */
DEFGFX("defface", Gdefface, 1, 64) {
    (void)env;
    Lobj name = CAR(args);
    if (!IS_SYMBOL(name)) { lm_error("defface: first arg must be a symbol", name); }
    int id = face_alloc(name);
    if (id < 0) { lm_error("defface: face table full", name); }
    face_set_kw(id, CDR(args));
    return FIXNUM(id);
}

/* (set-face-attribute NAME :kw val ...) -> face id (a fixnum).
 * Identical to defface but named to match Emacs's set-face-attribute.
 * Updates an existing face in place; creates it if new. */
DEFGFX("set-face-attribute", Gsetfa, 1, 64) {
    (void)env;
    Lobj name = CAR(args);
    if (!IS_SYMBOL(name)) { lm_error("set-face-attribute: first arg must be a symbol", name); }
    int id = face_alloc(name);
    if (id < 0) { lm_error("set-face-attribute: face table full", name); }
    face_set_kw(id, CDR(args));
    return FIXNUM(id);
}

/* (face-id NAME) -> fixnum id, or 0 for unknown names.
 * A lightweight inspection helper: lets Lisp tests or init code verify that
 * (defface 'ansi-red ...) actually claimed a slot. */
DEFGFX("face-id", Gfaceid, 1, 1) {
    (void)env;
    return FIXNUM(gfx_face_id(CAR(args)));
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
            /* arrows: cook Up/Down into C-p / C-n -- vertico's navigation
             * and REPL history. */
            return make_cons(intern("ctrl"),
                   make_cons(FIXNUM(ev.code == 103 ? 'p' : 'n'), Qnil));
        }
        if (ev.value == 1 && (ev.code == 105 || ev.code == 106)) {
            /* Left/Right -> C-b / C-f: move point a char when editing a buffer
             * (Emacs's backward-char / forward-char). */
            return make_cons(intern("ctrl"),
                   make_cons(FIXNUM(ev.code == 105 ? 'b' : 'f'), Qnil));
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
    // Sized to BUF_CAP, not a token-ish 512: ansi-color-apply substrings each
    // run of a streamed output chunk (up to ~1 KiB), so a 512 cap would silently
    // truncate plain program output > 511 bytes. A substring of a buffer-backed
    // string cannot exceed BUF_CAP anyway.
    static char tmp[BUF_CAP];
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

/* (screenshot "/shot.ppm") -> dump the framebuffer as a binary PPM (P6)
 * to a file. The fb is just process memory and the root / is the persistent
 * ext2 filesystem, so the OS can keep evidence of what its screen looked like. */
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

/* ---- text-property primitives --------------------------------------------- */
/*
 * These five primitives expose the rd_core interval store to Lisp. They operate
 * on the SELECTED window's buffer (cur()). lm_gfx.c is only ever compiled into
 * lm.elf (never into the kernel), and lm.elf is always built with -DLM_BUILD,
 * so the rd.h declarations for rd_put_text_prop etc. are always visible here.
 * No extra LM_BUILD guard is needed in this file.
 */

/* (put-text-property START END PROP VAL) -> nil
 * Set PROP=VAL on every character in [START,END) in the current buffer,
 * merging with any existing properties (other keys preserved). */
DEFGFX("put-text-property", Gput_tp, 4, 4) {
    (void)env;
    rd_put_text_prop(cur(),
                     (int)req_fixnum(nth_arg(args, 0), "put-text-property: start"),
                     (int)req_fixnum(nth_arg(args, 1), "put-text-property: end"),
                     nth_arg(args, 2),
                     nth_arg(args, 3));
    return Qnil;
}

/* (get-text-property POS PROP) -> value or nil
 * Return the value of PROP at character position POS in the current buffer,
 * or nil if POS has no text properties or PROP is absent. */
DEFGFX("get-text-property", Gget_tp, 2, 2) {
    (void)env;
    return rd_get_text_prop(cur(),
                            (int)req_fixnum(nth_arg(args, 0), "get-text-property: pos"),
                            nth_arg(args, 1));
}

/* (text-properties-at POS) -> plist or nil
 * Return the full property list at POS, or nil if there are no text properties
 * there. The plist is (k1 v1 k2 v2 ...). */
DEFGFX("text-properties-at", Gtp_at, 1, 1) {
    (void)env;
    return rd_text_props_at(cur(),
                            (int)req_fixnum(CAR(args), "text-properties-at: pos"));
}

/* (set-text-properties START END PLIST) -> nil
 * Replace the entire property list over [START,END). Every interval in the
 * range gets plist as its new plist (no merge). Pass nil to clear all props. */
DEFGFX("set-text-properties", Gset_tp, 3, 3) {
    (void)env;
    rd_set_text_props(cur(),
                      (int)req_fixnum(nth_arg(args, 0), "set-text-properties: start"),
                      (int)req_fixnum(nth_arg(args, 1), "set-text-properties: end"),
                      nth_arg(args, 2));
    return Qnil;
}

/* (remove-text-properties START END PROPS) -> nil
 * Remove the named properties (listed as symbols in PROPS; values are ignored,
 * matching Emacs's remove-text-properties contract) from every interval
 * intersecting [START,END). Intervals whose plist empties are dropped. */
DEFGFX("remove-text-properties", Grem_tp, 3, 3) {
    (void)env;
    rd_remove_text_props(cur(),
                         (int)req_fixnum(nth_arg(args, 0), "remove-text-properties: start"),
                         (int)req_fixnum(nth_arg(args, 1), "remove-text-properties: end"),
                         nth_arg(args, 2));
    return Qnil;
}

/* ---- GC root hook --------------------------------------------------------- */
/*
 * Buffer text-property plists are reachable ONLY from the buffer's interval
 * array. The GC's mark phase starts from explicit roots (symtab, global_env,
 * the eval argument) and conservative stack scanning, but those roots don't
 * include buffer state -- the buffer lives in C, not in the Lisp heap.
 *
 * Without this hook, a GC between (put-text-property ...) and the next
 * redisplay could sweep away the face cons cells that the intervals reference,
 * producing a dangling pointer in the interval plist. This is exactly the same
 * issue that makes real Emacs trace buffer text properties in its mark phase.
 *
 * gfx_gc_mark_buffers() is called from lm_core.c's gc_collect (under
 * #ifdef LM_BUILD) so it runs as part of the mark phase before gc_sweep.
 * The bufs[] array is static in this file, so this function is the natural
 * place to own the enumeration.
 */
void gfx_gc_mark_buffers(void)
{
    /* Mark every live buffer's text-property plists so the GC cannot sweep
     * face cons cells that are still referenced by interval plists. */
    for (int i = 0; i < NBUFS; i++) {
        if (buf_used[i]) { rd_buf_mark_props(&bufs[i], gc_mark); }
    }
    /* Also mark the face name symbols stored in face_names[].
     * Interned symbols are ALREADY GC roots via symtab[], so this is a
     * belt-and-suspenders guard against any future refactoring that might
     * change that invariant.  The cost is negligible (RD_NFACES mark calls). */
    for (int i = 0; i < RD_NFACES; i++) {
        gc_mark(face_names[i]);
    }
}

/* ---- registration ----------------------------------------------------------- */

void lm_gfx_register(void)
{
    register_Gframe_init();
    register_Gmake_buffer(); register_Gset_buffer(); register_Gcurrent_buffer();
    register_Gkill_buffer();
    register_Gset_mode_line_name();
    register_Gset_line_wrap();
    register_Ginsert(); register_Gdelete_char(); register_Gpropertize();
    register_Gpoint(); register_Gbuflen(); register_Ggoto_char(); register_Gbufsub();
    register_Gchar_at(); register_Gstring_ref();
    register_Gsplit_below(); register_Gsplit_right();
    register_Gother_window(); register_Gdelete_window();
    register_Gdelete_other(); register_Gbuffer_list();
    register_Gset_face(); register_Gdefface(); register_Gsetfa(); register_Gfaceid();
    register_Gecho(); register_Gselect_at();
    register_Gredisplay(); register_Gread_event();
    register_Gread_event_nb(); register_Gpoll_fd();
    register_Gframe_output();
    register_Gread_string(); register_Gprin1str(); register_Gstr_from_char();
    register_Gmake_surface(); register_Gsurf_fill(); register_Grun_in_buffer();
    register_Gfunction_info(); register_Gall_symbols(); register_Gstring_search();
    register_Gsubstring(); register_Gecho_select();
    register_Gscreenshot();
    /* text-property primitives (Phase 25.x: text properties + face support) */
    register_Gput_tp(); register_Gget_tp(); register_Gtp_at();
    register_Gset_tp(); register_Grem_tp();
}
