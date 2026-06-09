/*
 * lm.h -- Lisp Machine object representation (the shared "language").
 * =================================================================
 *
 * This is the soul of the Lisp side of MyOSv2. Every Lisp value is a single
 * 64-bit tagged word: the low 3 bits say what type it is, the upper bits are
 * either an immediate value (fixnums) or an 8-byte-aligned pointer to a heap
 * object. This is exactly how Emacs represents Lisp_Object -- C code and Lisp
 * code read and write the *same* bit patterns, with no marshalling.
 *
 * Ported from the standalone host interpreter at ~/Code/Sides/lm-lisp. The big
 * change for MyOSv2 is that this core is *freestanding*: no libc, no <stdio.h>.
 * All I/O goes through the Reader/Writer abstractions below, and all allocation
 * goes through lm_alloc()/lm_free(), which the *platform* provides (kmalloc in
 * the kernel test build, the user-space malloc in the /bin/lisp program). That
 * lets the identical core be compiled into BOTH the kernel (so the in-kernel
 * KTEST suite can red-green the reader/evaluator/GC) and the user ELF.
 */
#ifndef LM_H
#define LM_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * TAGGED POINTER SCHEME
 *
 * Every Lobj is a 64-bit integer whose low 3 bits are the tag:
 *
 *   000 (0) = fixnum    -- 61-bit signed integer in the upper bits
 *   001 (1) = cons      -- pointer to Cons
 *   010 (2) = symbol    -- pointer to Symbol
 *   011 (3) = string    -- pointer to LString
 *   100 (4) = primitive -- pointer to Prim (a C function, like an Emacs SUBR)
 *   101 (5) = lambda    -- pointer to a Cons holding (params . (body . env))
 *
 * All heap pointers are 8-byte aligned, so their low 3 bits are free for the
 * tag. The user-space malloc and the kernel heap both return >=16-byte-aligned
 * blocks, so this holds on MyOSv2.
 * ================================================================ */

typedef int64_t Lobj;

#define TAG_FIXNUM  0
#define TAG_CONS    1
#define TAG_SYMBOL  2
#define TAG_STRING  3
#define TAG_PRIM    4
#define TAG_LAMBDA  5

#define TAG_MASK    0x07
#define TAG_SHIFT   3

#define TAG(obj)    ((int)((obj) & TAG_MASK))
#define PTR(obj)    ((void *)((obj) & ~(Lobj)TAG_MASK))
#define TAGGED(ptr, tag)  ((Lobj)(((Lobj)(ptr)) | (tag)))

#define FIXNUM(n)       ((Lobj)(((Lobj)(n)) << TAG_SHIFT))
#define FIXNUM_VAL(obj) ((int64_t)(obj) >> TAG_SHIFT)

#define IS_FIXNUM(obj)  (TAG(obj) == TAG_FIXNUM)
#define IS_CONS(obj)    (TAG(obj) == TAG_CONS)
#define IS_SYMBOL(obj)  (TAG(obj) == TAG_SYMBOL)
#define IS_STRING(obj)  (TAG(obj) == TAG_STRING)
#define IS_PRIM(obj)    (TAG(obj) == TAG_PRIM)
#define IS_LAMBDA(obj)  (TAG(obj) == TAG_LAMBDA)

/* ================================================================
 * HEAP OBJECTS
 *
 * Every heap object starts with the same GC header: a `mark` byte for the
 * mark-and-sweep collector, a `type` byte (the tag, so the sweep phase knows
 * which owned data to free), and a `next` pointer chaining every allocated
 * object into one list the collector can walk.
 * ================================================================ */

#define GC_HEADER   \
    uint8_t mark;   \
    uint8_t type;   \
    void *next;

typedef struct Cons {
    GC_HEADER
    Lobj car;
    Lobj cdr;
} Cons;

typedef struct Symbol {
    GC_HEADER
    char *name;
    Lobj value;      /* variable binding (the "value slot") */
    Lobj function;   /* function binding (the "function slot" -- Lisp-2!) */
} Symbol;

typedef struct LString {
    GC_HEADER
    char *data;
    size_t len;
} LString;

/* A primitive: a C function exposed to Lisp, exactly like an Emacs DEFUN/SUBR. */
typedef Lobj (*PrimFn)(Lobj args, Lobj env);

typedef struct Prim {
    GC_HEADER
    const char *name;
    PrimFn fn;
    int min_args;
    int max_args;    /* -1 = variadic */
} Prim;

/* ================================================================
 * I/O ABSTRACTION (replaces FILE* from the host build)
 *
 * The host interpreter read with fgetc/ungetc on a FILE* and wrote with
 * fprintf. MyOSv2 has neither. A Reader is a character source -- either an
 * in-memory string or a file descriptor -- with a small pushback stack so the
 * recursive-descent reader can peek. A Writer is a character sink -- either a
 * capture buffer (used by the tests and by string-building) or a file
 * descriptor (the tty, a socket).
 * ================================================================ */

#define RD_STRING 0
#define RD_FD     1
#define LM_RD_PUSHBACK 8
#define LM_RD_LINE     512

typedef struct Reader {
    int kind;
    /* string source */
    const char *s;
    long pos, len;
    /* fd source */
    int fd;
    int tty;                    /* 1 = echo + line-edit on refill (a console) */
    unsigned char line[LM_RD_LINE];
    int llen, lpos;             /* refill buffer fill / cursor */
    int closed;                 /* fd source hit EOF */
    /* shared pushback */
    int pb[LM_RD_PUSHBACK];
    int pbn;
} Reader;

typedef struct Writer {
    char *buf;                  /* non-NULL => capture into buf[cap] */
    int cap, len;
    int fd;                     /* used when buf == NULL */
} Writer;

void reader_from_string(Reader *r, const char *s, long len);
void reader_from_fd(Reader *r, int fd, int tty);
int  rd_getc(Reader *r);
void rd_ungetc(Reader *r, int c);

void writer_to_buffer(Writer *w, char *buf, int cap);
void writer_to_fd(Writer *w, int fd);
void w_putc(Writer *w, int c);
void w_str(Writer *w, const char *s);
void w_long(Writer *w, int64_t v);

/* ================================================================
 * PLATFORM HOOKS (provided by the kernel or the user program)
 * ================================================================ */

void *lm_alloc(size_t size);   /* zeroed allocation */
void  lm_free(void *p);
long  lm_sys_read(int fd, void *buf, long n);
long  lm_sys_write(int fd, const void *buf, long n);
long  lm_open(const char *path);   /* open a file for `load`; <0 on failure */
void  lm_close(int fd);
void  lm_abort(void);              /* unrecoverable error (no jmp set) */

/* Conservative GC: the high end of the C stack, set once at program start.
 * When 0 (the kernel test build), GC skips stack scanning and relies only on
 * the explicit roots (symtab, global_env, the eval argument). */
extern uintptr_t lm_stack_base;

/* Tiny freestanding setjmp/longjmp (src/lm_jmp.S). Saves x19-x30 + sp, which
 * doubles as the register-root flush the conservative collector scans. */
typedef long lm_jmp_buf[16];
int  lm_setjmp(lm_jmp_buf);
void lm_longjmp(lm_jmp_buf, int);

/* ================================================================
 * SPECIAL CONSTANTS  (interned symbols compared by pointer identity)
 * ================================================================ */

extern Lobj Qnil, Qt, Qquote, Qlambda, Qif, Qdefun, Qsetq, Qprogn, Qwhile;
extern Lobj Qlet, Qdefmacro, Qand, Qor, Qcond, Qfunction;

#define IS_NIL(obj) ((obj) == Qnil)

/* ================================================================
 * GC + SYMBOL TABLE STATE
 * ================================================================ */

typedef struct {
    void *objects;          /* linked list of ALL heap objects */
    size_t alloc_count;
    size_t gc_threshold;    /* collect when alloc_count exceeds this */
    uintptr_t lo, hi;       /* address bounds of all objects (fast reject) */
} GC;

extern GC gc;

#define SYMTAB_SIZE 1024
extern Lobj symtab[SYMTAB_SIZE];
extern int symtab_count;
extern Lobj global_env;

/* ================================================================
 * CORE API
 * ================================================================ */

/* Memory & GC */
void *gc_alloc(size_t size, int tag);
void  gc_mark(Lobj obj);
void  gc_sweep(void);
void  gc_collect(Lobj env);

/* Constructors */
Lobj make_cons(Lobj car, Lobj cdr);
Lobj make_symbol(const char *name);
Lobj make_string(const char *data);
Lobj make_prim(const char *name, PrimFn fn, int min, int max);

/* Symbols / environment */
Lobj intern(const char *name);
Lobj env_lookup(Lobj sym, Lobj env);
Lobj env_set(Lobj sym, Lobj val, Lobj env);
Lobj env_extend(Lobj params, Lobj args, Lobj env);

/* List helpers */
static inline Lobj CAR(Lobj o) { return IS_CONS(o) ? ((Cons *)PTR(o))->car : Qnil; }
static inline Lobj CDR(Lobj o) { return IS_CONS(o) ? ((Cons *)PTR(o))->cdr : Qnil; }
static inline void SETCAR(Lobj o, Lobj v) { if (IS_CONS(o)) ((Cons *)PTR(o))->car = v; }
static inline void SETCDR(Lobj o, Lobj v) { if (IS_CONS(o)) ((Cons *)PTR(o))->cdr = v; }
int  list_length(Lobj list);
Lobj eval_list(Lobj list, Lobj env);

/* The big three */
Lobj lm_read(Reader *in);
Lobj lm_eval(Lobj expr, Lobj env);
void lm_print(Lobj obj, Writer *out);

/* Errors */
void lm_error(const char *msg, Lobj obj);

/* Bootstrap / convenience */
void lm_boot(void);                          /* build symbols + primitives + env */
Lobj lm_eval_cstr(const char *src);          /* read+eval the first form in src   */
Lobj lm_eval_all_str(const char *src);       /* read+eval every form; ret last val */
int  lm_eval_all(Reader *in);                /* read+eval every form; count forms */
int  lm_print_cstr(Lobj obj, char *buf, int cap);  /* print to buffer; ret length */
int  lm_repl_step(void);                     /* one read-eval-print turn; 0 at EOF */

/* Asynchronous Ctrl-C flag: the program's SIGINT handler sets this; the core
 * raises an "interrupted" error at the next safe point (inside gc_alloc). */
extern volatile int lm_sigint_pending;

/* The streams the I/O primitives (read/print/princ) act on. The REPL points
 * these at the tty (or a socket); the tests point them at buffers. */
extern Reader *lm_cur_in;
extern Writer *lm_cur_out;

#endif /* LM_H */
