/*
 * lm.c -- /bin/lisp, the MyOSv2 Lisp machine as a user program.
 * ============================================================
 *
 * This is the EL0 half of the Lisp machine. It links the same portable core as
 * the kernel (src/lm_core.c) but supplies the *user* platform layer: allocation
 * over the user-space malloc, and I/O over the kernel's syscalls (ulib). Then it
 * boots the image, loads the standard library from /lib/bootstrap.l, and runs an
 * interactive Read-Eval-Print loop on the serial console.
 *
 * The whole thing runs at EL0 behind the MMU like any other process -- exactly
 * the payoff of the "C kernel stays, Lisp is the userland" design.
 */

#include "lm.h"
#include "ulib.h"

/* ---- Platform layer: the hooks src/lm_core.c calls. ---- */

void *lm_alloc(size_t size)
{
    char *p = malloc(size);
    if (p) { for (size_t i = 0; i < size; i++) { p[i] = 0; } }  /* zero like calloc */
    return p;
}
void lm_free(void *p) { free(p); }

long lm_sys_read(int fd, void *buf, long n)        { return sys_read(fd, buf, n); }
long lm_sys_write(int fd, const void *buf, long n) { return sys_write(fd, buf, n); }
long lm_open(const char *path)                     { return sys_open(path); }
void lm_close(int fd)                              { sys_close(fd); }
void lm_abort(void)                                { sys_exit(1); }

/* ---- Ctrl-C: just raise the flag; the core acts on it at a safe point. ---- */
static void on_sigint(int sig) { (void)sig; lm_sigint_pending = 1; }

int umain(void)
{
    /* Record the high end of the C stack for the conservative collector. umain
     * is the outermost C frame (crt0 calls it), so every frame that can hold a
     * live Lisp object sits below this address. */
    volatile int stack_anchor;
    lm_stack_base = (uintptr_t)&stack_anchor;

    signal(SIGINT, on_sigint);

    lm_boot();

    static Writer out;
    writer_to_fd(&out, 1);
    lm_cur_out = &out;

    w_str(&out, "\nMyOSv2 Lisp Machine -- tagged objects, mark-sweep GC, Lisp-2.\n");

    /* Load the standard library (placed at /lib/bootstrap.l by the initrd). */
    lm_eval_all_str("(load \"/lib/bootstrap.l\")");

    /* Interactive REPL over the serial console (echo + line editing on refill). */
    static Reader in;
    reader_from_fd(&in, 0, 1);
    lm_cur_in = &in;

    for (;;) {
        w_str(&out, "lisp> ");
        if (!lm_repl_step()) { break; }
    }
    return 0;
}
