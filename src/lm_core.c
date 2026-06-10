/*
 * lm_core.c -- the portable heart of the MyOSv2 Lisp machine.
 * =========================================================
 *
 * Reader, evaluator, printer, garbage collector and the C primitives -- the
 * same architecture as Emacs's eval.c/alloc.c. This file is *platform-neutral*:
 * it never calls libc and never touches a kernel header. Everything it needs
 * from the outside world is a handful of hooks (lm_alloc/lm_free, lm_read/
 * lm_write/lm_open/lm_close, lm_setjmp/lm_longjmp) that the kernel build and the
 * user build each supply. That is what lets the identical core be linked into
 * the kernel (where KTEST exercises it) and into /bin/lisp.
 *
 * Ported from ~/Code/Sides/lm-lisp/lm.c. The evaluator, the special forms and
 * the primitive set are unchanged in spirit; the differences are all about
 * removing the hosted-libc assumptions (FILE*, malloc, setjmp, string.h) and
 * making the garbage collector scan C-stack roots conservatively so it is safe
 * to collect mid-computation in a process (init) that never exits.
 */

#include "lm.h"

/* ================================================================
 * GLOBALS
 * ================================================================ */

Lobj Qnil, Qt, Qquote, Qlambda, Qif, Qdefun, Qsetq, Qprogn, Qwhile;
Lobj Qunbound;   // the "no value" sentinel (see env_lookup)
Lobj Qlet, Qdefmacro, Qand, Qor, Qcond, Qfunction;

GC gc = { .objects = 0, .alloc_count = 0, .gc_threshold = 20000, .lo = 0, .hi = 0 };

Lobj symtab[SYMTAB_SIZE];
int symtab_count = 0;
Lobj global_env;

uintptr_t lm_stack_base = 0;

/* The current input/output streams the I/O primitives (read/print/princ) act on.
 * The REPL points these at the tty or a socket; the tests point them at buffers. */
Reader *lm_cur_in = 0;
Writer *lm_cur_out = 0;

/* Non-local exit target for lm_error, so a bad form unwinds the recursive
 * evaluator back to the REPL prompt instead of killing the process. */
static lm_jmp_buf error_jmp;
static int error_jmp_set = 0;

/* Set asynchronously by the program's SIGINT handler. We don't longjmp from the
 * handler itself (that would strand the kernel's signal frame); instead we poll
 * it from gc_alloc -- which runs on essentially every cons -- and raise a normal
 * error there, unwinding cleanly from ordinary C context back to the prompt. */
volatile int lm_sigint_pending = 0;

/* ================================================================
 * TINY FREESTANDING STRING HELPERS (no libc)
 * ================================================================ */

static size_t lm_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int lm_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char *lm_strdup(const char *s)
{
    size_t n = lm_strlen(s);
    char *p = lm_alloc(n + 1);
    for (size_t i = 0; i <= n; i++) { p[i] = s[i]; }
    return p;
}

static int lm_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Parse the whole token [buf] as a signed decimal integer. Returns 1 and stores
 * the value on success; 0 if the token is not a pure integer (=> it's a symbol). */
static int parse_fixnum(const char *buf, int len, int64_t *out)
{
    if (len == 0) { return 0; }
    int i = 0, neg = 0;
    if (buf[0] == '+' || buf[0] == '-') { neg = (buf[0] == '-'); i = 1; if (len == 1) return 0; }
    int64_t v = 0;
    for (; i < len; i++) {
        if (buf[i] < '0' || buf[i] > '9') { return 0; }
        v = v * 10 + (buf[i] - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

/* ================================================================
 * READER / WRITER (the FILE* replacement)
 * ================================================================ */

void reader_from_string(Reader *r, const char *s, long len)
{
    r->kind = RD_STRING; r->s = s; r->pos = 0; r->len = len;
    r->fd = -1; r->tty = 0; r->llen = 0; r->lpos = 0; r->closed = 0; r->pbn = 0;
}

void reader_from_fd(Reader *r, int fd, int tty)
{
    r->kind = RD_FD; r->s = 0; r->pos = 0; r->len = 0;
    r->fd = fd; r->tty = tty; r->llen = 0; r->lpos = 0; r->closed = 0; r->pbn = 0;
}

/* Refill the line buffer of an fd-backed Reader. For a tty we read a line with
 * echo and backspace editing (so the console behaves); for a raw fd (a file, a
 * socket) we just pull the next chunk. Returns 0 on EOF. */
static int reader_refill(Reader *r)
{
    if (r->closed) { return 0; }
    r->lpos = 0; r->llen = 0;
    if (r->tty) {
        int n = 0;
        for (;;) {
            char c;
            long got = lm_sys_read(r->fd, &c, 1);
            if (got != 1) { r->closed = 1; break; }
            if (c == '\r' || c == '\n') {
                lm_sys_write(1, "\n", 1);
                r->line[n++] = '\n';
                break;
            }
            if (c == 0x7f || c == 0x08) {            /* backspace */
                if (n > 0) { n--; lm_sys_write(1, "\b \b", 3); }
                continue;
            }
            if (n < LM_RD_LINE - 1) { r->line[n++] = c; lm_sys_write(1, &c, 1); }
        }
        r->llen = n;
    } else {
        long got = lm_sys_read(r->fd, r->line, LM_RD_LINE);
        if (got <= 0) { r->closed = 1; return 0; }
        r->llen = (int)got;
    }
    return r->llen > 0;
}

int rd_getc(Reader *r)
{
    if (r->pbn > 0) { return r->pb[--r->pbn]; }
    if (r->kind == RD_STRING) {
        if (r->pos < r->len) { return (unsigned char)r->s[r->pos++]; }
        return -1;
    }
    /* fd */
    if (r->lpos >= r->llen) {
        if (!reader_refill(r)) { return -1; }
    }
    return r->line[r->lpos++];
}

void rd_ungetc(Reader *r, int c)
{
    if (c >= 0 && r->pbn < LM_RD_PUSHBACK) { r->pb[r->pbn++] = c; }
}

void writer_to_buffer(Writer *w, char *buf, int cap)
{ w->buf = buf; w->cap = cap; w->len = 0; w->fd = -1; if (cap > 0) buf[0] = 0; }

void writer_to_fd(Writer *w, int fd)
{ w->buf = 0; w->cap = 0; w->len = 0; w->fd = fd; }

void w_putc(Writer *w, int c)
{
    if (w->buf) {
        if (w->len < w->cap - 1) { w->buf[w->len++] = (char)c; w->buf[w->len] = 0; }
    } else {
        char ch = (char)c;
        lm_sys_write(w->fd, &ch, 1);
    }
}

void w_str(Writer *w, const char *s) { while (*s) { w_putc(w, *s++); } }

void w_long(Writer *w, int64_t v)
{
    char tmp[24];
    int i = 0, neg = 0;
    uint64_t u;
    if (v < 0) { neg = 1; u = (uint64_t)(-(v + 1)) + 1; } else { u = (uint64_t)v; }
    if (u == 0) { tmp[i++] = '0'; }
    while (u > 0) { tmp[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) { w_putc(w, '-'); }
    while (i > 0) { w_putc(w, tmp[--i]); }
}

/* ================================================================
 * GARBAGE COLLECTOR -- mark and sweep, with conservative stack roots.
 * ================================================================ */

void *gc_alloc(size_t size, int tag)
{
    /* Collecting here (when over threshold) is safe ONLY because the collector
     * scans the C stack for roots -- so any half-built object the caller is
     * holding in a local survives. We gate auto-collect on lm_stack_base being
     * set, which is true in the real program but not in the KTEST build (where
     * collection is driven explicitly with known roots). */
    if (lm_stack_base && gc.alloc_count >= gc.gc_threshold) { gc_collect(global_env); }

    /* Honour a pending Ctrl-C at a safe point (see lm_sigint_pending). */
    if (lm_sigint_pending) { lm_sigint_pending = 0; lm_error("interrupted", Qnil); }

    Cons *obj = lm_alloc(size);            /* zeroed */
    obj->mark = 0;
    obj->type = (uint8_t)tag;
    obj->next = gc.objects;
    gc.objects = obj;
    gc.alloc_count++;

    uintptr_t a = (uintptr_t)obj;
    if (gc.lo == 0 || a < gc.lo) { gc.lo = a; }
    if (a + size > gc.hi) { gc.hi = a + size; }
    return obj;
}

/* Mark a heap object by its pointer, recursing on the *header* type (not on any
 * tag a conservative caller guessed) so misread bits can never drive recursion. */
static void gc_mark_ptr(void *ptr)
{
    if (!ptr) { return; }
    Cons *c = ptr;
    if (c->mark) { return; }
    c->mark = 1;
    switch (c->type) {
    case TAG_CONS:
    case TAG_LAMBDA:
        gc_mark(((Cons *)ptr)->car);
        gc_mark(((Cons *)ptr)->cdr);
        break;
    case TAG_SYMBOL:
        gc_mark(((Symbol *)ptr)->value);
        gc_mark(((Symbol *)ptr)->function);
        break;
    default: /* string, primitive: leaves */
        break;
    }
}

void gc_mark(Lobj obj)
{
    if (IS_FIXNUM(obj)) { return; }
    gc_mark_ptr(PTR(obj));
}

/* Is `ptr` the start of one of our live heap objects? Safe to call on any word
 * (it never dereferences `ptr`, only compares it). O(n) but fast-rejected by the
 * tracked address bounds, which makes stack scanning cheap in practice. */
static int is_heap_object(uintptr_t ptr)
{
    if (ptr < gc.lo || ptr >= gc.hi) { return 0; }
    for (void *o = gc.objects; o; o = ((Cons *)o)->next) {
        if ((uintptr_t)o == ptr) { return 1; }
    }
    return 0;
}

/* Treat every aligned word in [lo,hi) as a possible tagged Lisp pointer. */
static void gc_scan_words(uintptr_t lo, uintptr_t hi)
{
    lo = (lo + 7) & ~(uintptr_t)7;
    for (uintptr_t p = lo; p + sizeof(uintptr_t) <= hi; p += sizeof(uintptr_t)) {
        uintptr_t w = *(uintptr_t *)p;
        uintptr_t cand = w & ~(uintptr_t)TAG_MASK;
        if (is_heap_object(cand)) { gc_mark_ptr((void *)cand); }
    }
}

void gc_sweep(void)
{
    void **prev = &gc.objects;
    void *obj = gc.objects;
    while (obj) {
        void *next_obj = ((Cons *)obj)->next;
        if (!((Cons *)obj)->mark) {
            *prev = next_obj;
            if (((Cons *)obj)->type == TAG_STRING) {
                lm_free(((LString *)obj)->data);     /* free owned char data */
            }
            lm_free(obj);
            gc.alloc_count--;
        } else {
            ((Cons *)obj)->mark = 0;                 /* clear for next cycle */
            prev = &((Cons *)obj)->next;
        }
        obj = next_obj;
    }
}

void gc_collect(Lobj env)
{
    /* 1. Explicit roots: the obarray, the global environment, the eval arg. */
    for (int i = 0; i < symtab_count; i++) { gc_mark(symtab[i]); }
    gc_mark(global_env);
    gc_mark(env);

    /* 2. Conservative roots: flush callee-saved registers into a buffer (also a
     *    set of potential roots) and scan it plus the live C stack. */
    lm_jmp_buf regs;
    lm_setjmp(regs);
    gc_scan_words((uintptr_t)&regs[0], (uintptr_t)&regs[0] + sizeof(regs));
    if (lm_stack_base) {
        uintptr_t sp = (uintptr_t)&regs;             /* low end: our own frame */
        gc_scan_words(sp, lm_stack_base);
    }

    gc_sweep();
}

/* ================================================================
 * CONSTRUCTORS
 * ================================================================ */

Lobj make_cons(Lobj car, Lobj cdr)
{
    Cons *c = gc_alloc(sizeof(Cons), TAG_CONS);
    c->car = car; c->cdr = cdr;
    return TAGGED(c, TAG_CONS);
}

Lobj make_symbol(const char *name)
{
    Symbol *s = gc_alloc(sizeof(Symbol), TAG_SYMBOL);
    s->name = lm_strdup(name);
    s->value = Qunbound; s->function = Qnil;
    return TAGGED(s, TAG_SYMBOL);
}

Lobj make_string(const char *data)
{
    LString *s = gc_alloc(sizeof(LString), TAG_STRING);
    s->data = lm_strdup(data);
    s->len = lm_strlen(data);
    return TAGGED(s, TAG_STRING);
}

Lobj make_prim(const char *name, PrimFn fn, int min, int max)
{
    Prim *p = gc_alloc(sizeof(Prim), TAG_PRIM);
    p->name = name; p->fn = fn; p->min_args = min; p->max_args = max;
    return TAGGED(p, TAG_PRIM);
}

/* ================================================================
 * SYMBOL TABLE (OBARRAY)  -- intern() => one symbol per name.
 * ================================================================ */

Lobj intern(const char *name)
{
    for (int i = 0; i < symtab_count; i++) {
        Symbol *s = PTR(symtab[i]);
        if (lm_streq(s->name, name)) { return symtab[i]; }
    }
    if (symtab_count >= SYMTAB_SIZE) { lm_error("symbol table full", Qnil); }
    Lobj sym = make_symbol(name);
    symtab[symtab_count++] = sym;
    return sym;
}

/* ================================================================
 * ENVIRONMENT (association list; most recent binding wins)
 * ================================================================ */

Lobj env_lookup(Lobj sym, Lobj env)
{
    for (Lobj e = env; !IS_NIL(e); e = CDR(e)) {
        Lobj pair = CAR(e);
        if (CAR(pair) == sym) { return CDR(pair); }
    }
    Symbol *s = PTR(sym);
    // Qunbound (not Qnil) marks "never set": a global CAN legitimately hold
    // nil -- (setq flag nil) -- and must read back as nil, not as an error.
    if (s->value != Qunbound) { return s->value; }
    lm_error("unbound variable", sym);
    return Qnil;
}

Lobj env_set(Lobj sym, Lobj val, Lobj env)
{
    for (Lobj e = env; !IS_NIL(e); e = CDR(e)) {
        Lobj pair = CAR(e);
        if (CAR(pair) == sym) { SETCDR(pair, val); return val; }
    }
    ((Symbol *)PTR(sym))->value = val;       /* no local binding => global */
    return val;
}

Lobj env_extend(Lobj params, Lobj args, Lobj env)
{
    /* Bind the (proper-list) parameters to the arguments pairwise... */
    while (IS_CONS(params) && !IS_NIL(args)) {
        env = make_cons(make_cons(CAR(params), CAR(args)), env);
        params = CDR(params); args = CDR(args);
    }
    /* ...and if the parameter "list" is a bare SYMBOL instead -- (lambda args
     * ...) / (defun run args ...) -- bind that symbol to ALL the (remaining)
     * arguments as a list: the classic &rest, in its simplest spelling. The
     * Lisp shell needs this so (run "cmd" "a" "b" ...) can take any number of
     * arguments. nil is itself a symbol, so exclude it (an empty parameter
     * list must not bind anything). */
    if (IS_SYMBOL(params) && !IS_NIL(params)) {
        env = make_cons(make_cons(params, args), env);
    }
    return env;
}

/* ================================================================
 * LIST UTILITIES
 * ================================================================ */

int list_length(Lobj list) { int n = 0; while (!IS_NIL(list)) { n++; list = CDR(list); } return n; }

Lobj eval_list(Lobj list, Lobj env)
{
    if (IS_NIL(list)) { return Qnil; }
    Lobj head = lm_eval(CAR(list), env);
    return make_cons(head, eval_list(CDR(list), env));
}

/* ================================================================
 * READER
 * ================================================================ */

static int peek_char(Reader *in) { int c = rd_getc(in); rd_ungetc(in, c); return c; }

static void skip_whitespace(Reader *in)
{
    int c;
    while ((c = rd_getc(in)) != -1) {
        if (c == ';') { while ((c = rd_getc(in)) != -1 && c != '\n') {} continue; }
        if (!lm_isspace(c)) { rd_ungetc(in, c); return; }
    }
}

static Lobj read_string(Reader *in)
{
    char buf[4096];
    int i = 0, c;
    while ((c = rd_getc(in)) != -1 && c != '"' && i < (int)sizeof(buf) - 1) {
        if (c == '\\') {
            c = rd_getc(in);
            switch (c) {
            case 'n': buf[i++] = '\n'; break;
            case 't': buf[i++] = '\t'; break;
            case '\\': buf[i++] = '\\'; break;
            case '"': buf[i++] = '"'; break;
            default: buf[i++] = (char)c; break;
            }
        } else { buf[i++] = (char)c; }
    }
    buf[i] = 0;
    return make_string(buf);
}

static Lobj read_atom(Reader *in)
{
    char buf[256];
    int i = 0, c;
    while ((c = rd_getc(in)) != -1 && !lm_isspace(c) &&
           c != '(' && c != ')' && c != '"' && c != ';') {
        if (i < (int)sizeof(buf) - 1) { buf[i++] = (char)c; }
    }
    buf[i] = 0;
    if (c != -1) { rd_ungetc(in, c); }

    int64_t val;
    if (parse_fixnum(buf, i, &val)) { return FIXNUM(val); }
    return intern(buf);
}

static Lobj read_list(Reader *in)
{
    skip_whitespace(in);
    int c = peek_char(in);
    if (c == ')') { rd_getc(in); return Qnil; }
    if (c == -1) { lm_error("unexpected end of input in list", Qnil); return Qnil; }
    Lobj car = lm_read(in);
    Lobj cdr = read_list(in);
    return make_cons(car, cdr);
}

Lobj lm_read(Reader *in)
{
    skip_whitespace(in);
    int c = rd_getc(in);
    if (c == -1) { return Qnil; }
    switch (c) {
    case '(':
        return read_list(in);
    case '\'': {
        Lobj quoted = lm_read(in);
        return make_cons(Qquote, make_cons(quoted, Qnil));
    }
    case '#': {
        int next = rd_getc(in);
        if (next == '\'') {
            Lobj func = lm_read(in);
            return make_cons(Qfunction, make_cons(func, Qnil));
        }
        lm_error("unknown # dispatch", Qnil);
        return Qnil;
    }
    case '"':
        return read_string(in);
    case ')':
        lm_error("unexpected )", Qnil);
        return Qnil;
    default:
        rd_ungetc(in, c);
        return read_atom(in);
    }
}

/* ================================================================
 * PRINTER
 * ================================================================ */

void lm_print(Lobj obj, Writer *out)
{
    switch (TAG(obj)) {
    case TAG_FIXNUM:
        w_long(out, FIXNUM_VAL(obj));
        break;
    case TAG_CONS: {
        w_putc(out, '(');
        Lobj cur = obj;
        int first = 1;
        while (IS_CONS(cur)) {
            if (!first) { w_putc(out, ' '); }
            first = 0;
            lm_print(CAR(cur), out);
            cur = CDR(cur);
        }
        if (!IS_NIL(cur)) { w_str(out, " . "); lm_print(cur, out); }
        w_putc(out, ')');
        break;
    }
    case TAG_SYMBOL:
        w_str(out, ((Symbol *)PTR(obj))->name);
        break;
    case TAG_STRING:
        w_putc(out, '"'); w_str(out, ((LString *)PTR(obj))->data); w_putc(out, '"');
        break;
    case TAG_PRIM:
        w_str(out, "#<primitive:"); w_str(out, ((Prim *)PTR(obj))->name); w_putc(out, '>');
        break;
    case TAG_LAMBDA:
        w_str(out, "#<lambda>");
        break;
    default:
        w_str(out, "#<unknown>");
        break;
    }
}

/* ================================================================
 * EVALUATOR -- the heart of the machine (with tail-call optimization).
 * ================================================================ */

Lobj lm_eval(Lobj expr, Lobj env)
{
tail_call:
    if (IS_FIXNUM(expr) || IS_STRING(expr)) { return expr; }

    if (IS_SYMBOL(expr)) {
        if (expr == Qnil) { return Qnil; }
        if (expr == Qt) { return Qt; }
        return env_lookup(expr, env);
    }

    if (!IS_CONS(expr)) { lm_error("cannot evaluate", expr); return Qnil; }

    Lobj head = CAR(expr);
    Lobj rest = CDR(expr);

    /* ---- special forms ---- */
    if (head == Qquote) { return CAR(rest); }

    if (head == Qfunction) {
        Lobj arg = CAR(rest);
        if (IS_SYMBOL(arg)) {
            Lobj func = ((Symbol *)PTR(arg))->function;
            if (IS_NIL(func)) { lm_error("void function", arg); }
            return func;
        }
        return lm_eval(arg, env);
    }

    if (head == Qif) {
        Lobj cond = lm_eval(CAR(rest), env);
        expr = !IS_NIL(cond) ? CAR(CDR(rest)) : CAR(CDR(CDR(rest)));
        goto tail_call;
    }

    if (head == Qprogn) {
        if (IS_NIL(rest)) { return Qnil; }
        while (!IS_NIL(CDR(rest))) { lm_eval(CAR(rest), env); rest = CDR(rest); }
        expr = CAR(rest);
        goto tail_call;
    }

    if (head == Qsetq) {
        Lobj sym = CAR(rest);
        Lobj val = lm_eval(CAR(CDR(rest)), env);
        env_set(sym, val, env);
        return val;
    }

    if (head == Qdefun) {
        Lobj name = CAR(rest);
        Lobj params = CAR(CDR(rest));
        Lobj body = CDR(CDR(rest));
        Lobj lambda_body = IS_NIL(CDR(body)) ? CAR(body) : make_cons(Qprogn, body);
        Cons *lc = gc_alloc(sizeof(Cons), TAG_LAMBDA);
        lc->car = params;
        lc->cdr = make_cons(lambda_body, env);
        ((Symbol *)PTR(name))->function = TAGGED(lc, TAG_LAMBDA);
        return name;
    }

    if (head == Qdefmacro) {
        Lobj name = CAR(rest);
        Lobj params = CAR(CDR(rest));
        Lobj body = CDR(CDR(rest));
        Lobj macro_body = IS_NIL(CDR(body)) ? CAR(body) : make_cons(Qprogn, body);
        Cons *mc = gc_alloc(sizeof(Cons), TAG_LAMBDA);
        mc->car = params;
        mc->cdr = make_cons(macro_body, env);
        Lobj macro_marker = make_cons(intern("macro"), TAGGED(mc, TAG_LAMBDA));
        ((Symbol *)PTR(name))->function = macro_marker;
        return name;
    }

    if (head == Qlambda) {
        Lobj params = CAR(rest);
        Lobj body = CDR(rest);
        Lobj lambda_body = IS_NIL(CDR(body)) ? CAR(body) : make_cons(Qprogn, body);
        Cons *lc = gc_alloc(sizeof(Cons), TAG_LAMBDA);
        lc->car = params;
        lc->cdr = make_cons(lambda_body, env);
        return TAGGED(lc, TAG_LAMBDA);
    }

    if (head == Qwhile) {
        Lobj cond_expr = CAR(rest);
        Lobj body = CDR(rest);
        Lobj result = Qnil;
        while (!IS_NIL(lm_eval(cond_expr, env))) {
            for (Lobj b = body; !IS_NIL(b); b = CDR(b)) { result = lm_eval(CAR(b), env); }
        }
        return result;
    }

    if (head == Qlet) {
        Lobj bindings = CAR(rest);
        Lobj body = CDR(rest);
        Lobj new_env = env;
        while (!IS_NIL(bindings)) {
            Lobj binding = CAR(bindings);
            Lobj val = lm_eval(CAR(CDR(binding)), env);
            new_env = make_cons(make_cons(CAR(binding), val), new_env);
            bindings = CDR(bindings);
        }
        if (IS_NIL(body)) { return Qnil; }
        while (!IS_NIL(CDR(body))) { lm_eval(CAR(body), new_env); body = CDR(body); }
        env = new_env; expr = CAR(body);
        goto tail_call;
    }

    if (head == Qand) {
        Lobj result = Qt;
        for (Lobj f = rest; !IS_NIL(f); f = CDR(f)) {
            result = lm_eval(CAR(f), env);
            if (IS_NIL(result)) { return Qnil; }
        }
        return result;
    }

    if (head == Qor) {
        for (Lobj f = rest; !IS_NIL(f); f = CDR(f)) {
            Lobj result = lm_eval(CAR(f), env);
            if (!IS_NIL(result)) { return result; }
        }
        return Qnil;
    }

    if (head == Qcond) {
        for (Lobj clauses = rest; !IS_NIL(clauses); clauses = CDR(clauses)) {
            Lobj clause = CAR(clauses);
            Lobj test = lm_eval(CAR(clause), env);
            if (!IS_NIL(test)) {
                Lobj body = CDR(clause);
                if (IS_NIL(body)) { return test; }
                while (!IS_NIL(CDR(body))) { lm_eval(CAR(body), env); body = CDR(body); }
                expr = CAR(body);
                goto tail_call;
            }
        }
        return Qnil;
    }

    /* ---- function call ---- */
    Lobj func;
    if (IS_SYMBOL(head)) {
        func = ((Symbol *)PTR(head))->function;
        if (IS_CONS(func)) {                       /* macro? */
            Lobj marker = CAR(func);
            if (IS_SYMBOL(marker) && lm_streq(((Symbol *)PTR(marker))->name, "macro")) {
                Cons *mc = PTR(CDR(func));
                Lobj new_env = env_extend(mc->car, rest, CDR(mc->cdr));
                expr = lm_eval(CAR(mc->cdr), new_env);
                goto tail_call;
            }
        }
        if (IS_NIL(func)) { func = env_lookup(head, env); }
    } else {
        func = lm_eval(head, env);
    }

    Lobj evaled_args = eval_list(rest, env);

    if (IS_PRIM(func)) {
        Prim *p = PTR(func);
        return p->fn(evaled_args, env);
    }
    if (IS_LAMBDA(func)) {
        Cons *lc = PTR(func);
        env = env_extend(lc->car, evaled_args, CDR(lc->cdr));
        expr = CAR(lc->cdr);
        goto tail_call;
    }
    lm_error("not a function", head);
    return Qnil;
}

/* ================================================================
 * ERRORS
 * ================================================================ */

void lm_error(const char *msg, Lobj obj)
{
    /* Report on the CURRENT output stream, not blindly on fd 2: when the REPL
     * is a TCP socket (24.1b), the person who typed the bad form is on the
     * other end of that socket, and a message on the guest's serial console
     * would be invisible to them. Fall back to the raw stderr fd only when no
     * stream is set up yet (e.g. an error while loading the bootstrap). */
    Writer fallback;
    Writer *w = lm_cur_out;
    if (!w) { writer_to_fd(&fallback, 2); w = &fallback; }
    w_str(w, "ERROR: "); w_str(w, msg);
    if (obj != Qnil) { w_str(w, " -- "); lm_print(obj, w); }
    w_putc(w, '\n');
    if (error_jmp_set) { lm_longjmp(error_jmp, 1); }
    lm_abort();
}

/* ================================================================
 * PRIMITIVES (SUBR)  -- C functions exposed to Lisp via DEFUN.
 * ================================================================ */

#define DEFUN(lisp_name, c_name, min, max)                        \
    static Lobj c_name(Lobj args, Lobj env);                      \
    static void register_##c_name(void) {                         \
        Lobj sym = intern(lisp_name);                             \
        ((Symbol *)PTR(sym))->function = make_prim(lisp_name, c_name, min, max); \
    }                                                             \
    static Lobj c_name(Lobj args, Lobj env)

DEFUN("+", Fadd, 0, -1) {
    (void)env; int64_t s = 0;
    while (!IS_NIL(args)) { s += FIXNUM_VAL(CAR(args)); args = CDR(args); }
    return FIXNUM(s);
}
DEFUN("-", Fsub, 1, -1) {
    (void)env; int64_t v = FIXNUM_VAL(CAR(args)); args = CDR(args);
    if (IS_NIL(args)) { return FIXNUM(-v); }
    while (!IS_NIL(args)) { v -= FIXNUM_VAL(CAR(args)); args = CDR(args); }
    return FIXNUM(v);
}
DEFUN("*", Fmul, 0, -1) {
    (void)env; int64_t p = 1;
    while (!IS_NIL(args)) { p *= FIXNUM_VAL(CAR(args)); args = CDR(args); }
    return FIXNUM(p);
}
DEFUN("/", Fdiv, 2, 2) {
    (void)env; int64_t a = FIXNUM_VAL(CAR(args)), b = FIXNUM_VAL(CAR(CDR(args)));
    if (b == 0) { lm_error("division by zero", Qnil); }
    return FIXNUM(a / b);
}
DEFUN("%", Fmod, 2, 2) {
    (void)env; int64_t a = FIXNUM_VAL(CAR(args)), b = FIXNUM_VAL(CAR(CDR(args)));
    if (b == 0) { lm_error("modulo by zero", Qnil); }
    return FIXNUM(a % b);
}
DEFUN("=", Fnumeq, 2, 2) { (void)env; return FIXNUM_VAL(CAR(args)) == FIXNUM_VAL(CAR(CDR(args))) ? Qt : Qnil; }
DEFUN("<", Flt, 2, 2)   { (void)env; return FIXNUM_VAL(CAR(args)) <  FIXNUM_VAL(CAR(CDR(args))) ? Qt : Qnil; }
DEFUN(">", Fgt, 2, 2)   { (void)env; return FIXNUM_VAL(CAR(args)) >  FIXNUM_VAL(CAR(CDR(args))) ? Qt : Qnil; }
DEFUN("<=", Flte, 2, 2) { (void)env; return FIXNUM_VAL(CAR(args)) <= FIXNUM_VAL(CAR(CDR(args))) ? Qt : Qnil; }
DEFUN(">=", Fgte, 2, 2) { (void)env; return FIXNUM_VAL(CAR(args)) >= FIXNUM_VAL(CAR(CDR(args))) ? Qt : Qnil; }

DEFUN("eq", Feq, 2, 2) { (void)env; return CAR(args) == CAR(CDR(args)) ? Qt : Qnil; }
DEFUN("equal", Fequal, 2, 2) {
    (void)env; Lobj a = CAR(args), b = CAR(CDR(args));
    if (a == b) { return Qt; }
    if (TAG(a) != TAG(b)) { return Qnil; }
    if (IS_STRING(a)) {
        return lm_streq(((LString *)PTR(a))->data, ((LString *)PTR(b))->data) ? Qt : Qnil;
    }
    return Qnil;
}
DEFUN("cons", Fcons, 2, 2) { (void)env; return make_cons(CAR(args), CAR(CDR(args))); }
DEFUN("car", Fcar, 1, 1) { (void)env; return CAR(CAR(args)); }
DEFUN("cdr", Fcdr, 1, 1) { (void)env; return CDR(CAR(args)); }
DEFUN("setcar", Fsetcar, 2, 2) { (void)env; SETCAR(CAR(args), CAR(CDR(args))); return CAR(CDR(args)); }
DEFUN("setcdr", Fsetcdr, 2, 2) { (void)env; SETCDR(CAR(args), CAR(CDR(args))); return CAR(CDR(args)); }
DEFUN("list", Flist, 0, -1) { (void)env; return args; }

DEFUN("null", Fnull, 1, 1) { (void)env; return IS_NIL(CAR(args)) ? Qt : Qnil; }
DEFUN("atom", Fatom, 1, 1) { (void)env; return IS_CONS(CAR(args)) ? Qnil : Qt; }
DEFUN("consp", Fconsp, 1, 1) { (void)env; return IS_CONS(CAR(args)) ? Qt : Qnil; }
DEFUN("numberp", Fnumberp, 1, 1) { (void)env; return IS_FIXNUM(CAR(args)) ? Qt : Qnil; }
DEFUN("symbolp", Fsymbolp, 1, 1) { (void)env; return IS_SYMBOL(CAR(args)) ? Qt : Qnil; }
DEFUN("stringp", Fstringp, 1, 1) { (void)env; return IS_STRING(CAR(args)) ? Qt : Qnil; }

DEFUN("string-length", Fstrlen, 1, 1) {
    (void)env; Lobj s = CAR(args);
    if (!IS_STRING(s)) { lm_error("not a string", s); }
    return FIXNUM((int64_t)((LString *)PTR(s))->len);
}
DEFUN("string-concat", Fstrcat, 0, -1) {
    (void)env; char buf[2048]; int n = 0;
    while (!IS_NIL(args)) {
        Lobj s = CAR(args);
        if (IS_STRING(s)) {
            const char *d = ((LString *)PTR(s))->data;
            while (*d && n < (int)sizeof(buf) - 1) { buf[n++] = *d++; }
        } else if (IS_FIXNUM(s)) {
            Writer w; char tmp[24]; writer_to_buffer(&w, tmp, sizeof(tmp));
            w_long(&w, FIXNUM_VAL(s));
            for (int i = 0; tmp[i] && n < (int)sizeof(buf) - 1; i++) { buf[n++] = tmp[i]; }
        }
        args = CDR(args);
    }
    buf[n] = 0;
    return make_string(buf);
}
DEFUN("symbol-name", Fsymname, 1, 1) {
    (void)env; Lobj s = CAR(args);
    if (!IS_SYMBOL(s)) { lm_error("not a symbol", s); }
    return make_string(((Symbol *)PTR(s))->name);
}

DEFUN("print", Fprint, 1, 1) {
    (void)env;
    if (lm_cur_out) { lm_print(CAR(args), lm_cur_out); w_putc(lm_cur_out, '\n'); }
    return CAR(args);
}
DEFUN("princ", Fprinc, 1, 1) {
    (void)env; Lobj o = CAR(args);
    if (lm_cur_out) {
        if (IS_STRING(o)) { w_str(lm_cur_out, ((LString *)PTR(o))->data); }
        else { lm_print(o, lm_cur_out); }
    }
    return o;
}
DEFUN("terpri", Fterpri, 0, 0) { (void)env; (void)args; if (lm_cur_out) { w_putc(lm_cur_out, '\n'); } return Qnil; }
DEFUN("read", Fread, 0, 0) { (void)env; (void)args; return lm_cur_in ? lm_read(lm_cur_in) : Qnil; }

DEFUN("load", Fload, 1, 1) {
    (void)env; Lobj path_obj = CAR(args);
    if (!IS_STRING(path_obj)) { lm_error("load: expected string", path_obj); }
    const char *path = ((LString *)PTR(path_obj))->data;
    long fd = lm_open(path);
    if (fd < 0) { lm_error("load: cannot open file", path_obj); return Qnil; }
    Reader r; reader_from_fd(&r, (int)fd, 0);
    lm_eval_all(&r);
    lm_close((int)fd);
    return Qt;
}

/* (eval form) -- the missing third of read/eval/print. With this, a REPL can
 * be written IN Lisp: (print (eval (read))). The form is evaluated in the
 * caller's environment, so locals are visible to constructed code. */
DEFUN("eval", Feval, 1, 1) {
    return lm_eval(CAR(args), env);
}

DEFUN("apply", Fapply, 2, 2) {
    Lobj func = CAR(args), fargs = CAR(CDR(args));
    if (IS_PRIM(func)) { return ((Prim *)PTR(func))->fn(fargs, env); }
    if (IS_LAMBDA(func)) {
        Cons *lc = PTR(func);
        return lm_eval(CAR(lc->cdr), env_extend(lc->car, fargs, CDR(lc->cdr)));
    }
    lm_error("apply: not a function", func);
    return Qnil;
}
DEFUN("funcall", Ffuncall, 1, -1) {
    Lobj func = CAR(args), fargs = CDR(args);
    if (IS_PRIM(func)) { return ((Prim *)PTR(func))->fn(fargs, env); }
    if (IS_LAMBDA(func)) {
        Cons *lc = PTR(func);
        return lm_eval(CAR(lc->cdr), env_extend(lc->car, fargs, CDR(lc->cdr)));
    }
    lm_error("funcall: not a function", func);
    return Qnil;
}
DEFUN("mapcar", Fmapcar, 2, 2) {
    Lobj func = CAR(args), lst = CAR(CDR(args));
    Lobj result = Qnil, tail = Qnil;
    while (!IS_NIL(lst)) {
        Lobj call_args = make_cons(CAR(lst), Qnil), val;
        if (IS_PRIM(func)) { val = ((Prim *)PTR(func))->fn(call_args, env); }
        else if (IS_LAMBDA(func)) {
            Cons *lc = PTR(func);
            val = lm_eval(CAR(lc->cdr), env_extend(lc->car, call_args, CDR(lc->cdr)));
        } else { lm_error("mapcar: not a function", func); return Qnil; }
        Lobj cell = make_cons(val, Qnil);
        if (IS_NIL(result)) { result = cell; } else { SETCDR(tail, cell); }
        tail = cell;
        lst = CDR(lst);
    }
    return result;
}

DEFUN("gc", Fgc, 0, 0) { (void)args; gc_collect(env); return FIXNUM((int64_t)gc.alloc_count); }
DEFUN("type-of", Ftypeof, 1, 1) {
    (void)env;
    switch (TAG(CAR(args))) {
    case TAG_FIXNUM: return intern("fixnum");
    case TAG_CONS:   return intern("cons");
    case TAG_SYMBOL: return intern("symbol");
    case TAG_STRING: return intern("string");
    case TAG_PRIM:   return intern("primitive");
    case TAG_LAMBDA: return intern("lambda");
    default:         return intern("unknown");
    }
}
DEFUN("error", Ferror, 1, 1) {
    (void)env; Lobj msg = CAR(args);
    if (IS_STRING(msg)) { lm_error(((LString *)PTR(msg))->data, Qnil); }
    else { lm_error("error", msg); }
    return Qnil;
}
DEFUN("not", Fnot, 1, 1) { (void)env; return IS_NIL(CAR(args)) ? Qt : Qnil; }

/* ================================================================
 * BOOTSTRAP
 * ================================================================ */

static void register_primitives(void)
{
    register_Fadd(); register_Fsub(); register_Fmul(); register_Fdiv(); register_Fmod();
    register_Fnumeq(); register_Flt(); register_Fgt(); register_Flte(); register_Fgte();
    register_Feq(); register_Fequal(); register_Fcons(); register_Fcar(); register_Fcdr();
    register_Fsetcar(); register_Fsetcdr(); register_Flist();
    register_Fnull(); register_Fatom(); register_Fconsp(); register_Fnumberp();
    register_Fsymbolp(); register_Fstringp();
    register_Fstrlen(); register_Fstrcat(); register_Fsymname();
    register_Fprint(); register_Fprinc(); register_Fterpri(); register_Fread(); register_Fload();
    register_Feval(); register_Fapply(); register_Ffuncall(); register_Fmapcar();
    register_Fgc(); register_Ftypeof(); register_Ferror(); register_Fnot();
}

void lm_boot(void)
{
    /* Reset all state, so the kernel KTEST suite can boot a fresh image per run. */
    gc.objects = 0; gc.alloc_count = 0; gc.lo = 0; gc.hi = 0;
    symtab_count = 0; global_env = 0;

    // The unbound sentinel must exist before anything else is interned, and
    // before Qnil exists make_symbol writes garbage into value slots -- so
    // intern it first and repair its own slot, then give nil and t theirs.
    Qunbound = intern("--unbound--");
    ((Symbol *)PTR(Qunbound))->value = Qunbound;
    Qnil = intern("nil");
    ((Symbol *)PTR(Qnil))->value = Qnil;
    Qt = intern("t");
    ((Symbol *)PTR(Qt))->value = Qt;
    Qquote = intern("quote"); Qlambda = intern("lambda"); Qif = intern("if");
    Qdefun = intern("defun"); Qsetq = intern("setq"); Qprogn = intern("progn");
    Qwhile = intern("while"); Qlet = intern("let"); Qdefmacro = intern("defmacro");
    Qand = intern("and"); Qor = intern("or"); Qcond = intern("cond");
    Qfunction = intern("function");

    register_primitives();
    global_env = Qnil;
}

/* ================================================================
 * CONVENIENCE ENTRY POINTS (used by the REPL and the tests)
 * ================================================================ */

/* Read+eval every form from `in`. Returns how many forms were evaluated. */
int lm_eval_all(Reader *in)
{
    int n = 0;
    for (;;) {
        skip_whitespace(in);
        if (peek_char(in) == -1) { break; }
        Lobj e = lm_read(in);
        lm_eval(e, global_env);
        n++;
    }
    return n;
}

/* Read+eval the first form of a C string, with error recovery. Returns the
 * value, or Qnil if the form errored. */
Lobj lm_eval_cstr(const char *src)
{
    Reader r; reader_from_string(&r, src, (long)lm_strlen(src));
    int prev = error_jmp_set; lm_jmp_buf saved;
    for (int i = 0; i < 16; i++) { saved[i] = error_jmp[i]; }
    Lobj result = Qnil;
    if (lm_setjmp(error_jmp) == 0) {
        error_jmp_set = 1;
        result = lm_eval(lm_read(&r), global_env);
    }
    error_jmp_set = prev;
    for (int i = 0; i < 16; i++) { error_jmp[i] = saved[i]; }
    return result;
}

int lm_print_cstr(Lobj obj, char *buf, int cap)
{
    Writer w; writer_to_buffer(&w, buf, cap);
    lm_print(obj, &w);
    return w.len;
}

/* One turn of the Read-Eval-Print loop, reading from lm_cur_in and writing to
 * lm_cur_out, under the core's own error-recovery point. The caller prints the
 * prompt and supplies the streams. Returns 0 at end-of-input, 1 otherwise. A
 * form that errors (or a Ctrl-C) is caught here: the message was already emitted
 * by lm_error, and we simply return to let the caller prompt again. */
int lm_repl_step(void)
{
    if (!lm_cur_in) { return 0; }
    skip_whitespace(lm_cur_in);
    if (peek_char(lm_cur_in) == -1) { return 0; }
    if (lm_setjmp(error_jmp) == 0) {
        error_jmp_set = 1;
        Lobj val = lm_eval(lm_read(lm_cur_in), global_env);
        if (lm_cur_out) { lm_print(val, lm_cur_out); w_putc(lm_cur_out, '\n'); }
    }
    error_jmp_set = 0;
    return 1;
}

/* Read+eval EVERY form of a C string under one recovery point; return the last
 * value (Qnil if a form errored). Used by tests and by the REPL's batch loads. */
Lobj lm_eval_all_str(const char *src)
{
    Reader r; reader_from_string(&r, src, (long)lm_strlen(src));
    int prev = error_jmp_set; lm_jmp_buf saved;
    for (int i = 0; i < 16; i++) { saved[i] = error_jmp[i]; }
    Lobj last = Qnil;
    if (lm_setjmp(error_jmp) == 0) {
        error_jmp_set = 1;
        for (;;) {
            skip_whitespace(&r);
            if (peek_char(&r) == -1) { break; }
            last = lm_eval(lm_read(&r), global_env);
        }
    }
    error_jmp_set = prev;
    for (int i = 0; i < 16; i++) { error_jmp[i] = saved[i]; }
    return last;
}
