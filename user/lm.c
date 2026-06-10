/*
 * lm.c -- /bin/lisp, the MyOSv2 Lisp machine as a user program.
 * ============================================================
 *
 * This is the EL0 half of the Lisp machine. It links the same portable core as
 * the kernel (src/lm_core.c) but supplies the *user* platform layer: allocation
 * over the user-space malloc, and I/O over the kernel's syscalls (ulib). Then it
 * boots the image, loads the standard library from /lib/bootstrap.l, and runs a
 * Read-Eval-Print loop.
 *
 * Two ways to talk to the image:
 *
 *   lisp                  interactive REPL on the serial console
 *   lisp -serve [port]    network REPL: listen on `port` (default 7777) and
 *                         serve one connection at a time. The IMAGE PERSISTS
 *                         across connections -- disconnect, reconnect, and your
 *                         defuns are still there. This is the Doom Emacs path
 *                         (see user/lisp/lm-mode.el): you eval forms from your
 *                         editor into the live guest, the Lisp-machine way.
 *
 * The whole thing runs at EL0 behind the MMU like any other process -- exactly
 * the payoff of the "C kernel stays, Lisp is the userland" design.
 */

#include "lm.h"
#include "ulib.h"
#include "lm_sys.h"

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

/* ---- Tiny arg helpers (no libc here). ---- */

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Parse a decimal port number. Returns -1 on anything that isn't a clean
 * 1..65535 -- a mistyped port should be an error, not a surprise bind(0). */
static int parse_port(const char *s)
{
    long v = 0;
    if (!*s) { return -1; }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { return -1; }
        v = v * 10 + (*s - '0');
        if (v > 65535) { return -1; }
    }
    return (v >= 1) ? (int)v : -1;
}

static const char *BANNER =
    "\nMyOSv2 Lisp Machine -- tagged objects, mark-sweep GC, Lisp-2.\n";

/* Drive the REPL until end-of-input: prompt, read one form, eval, print.
 * lm_repl_step() owns the error recovery -- a bad form prints an ERROR line
 * (through lm_cur_out, so a remote user sees it) and we just prompt again. */
static void repl_loop(Writer *out)
{
    for (;;) {
        w_str(out, "lisp> ");
        if (!lm_repl_step()) { break; }
    }
}

/*
 * The network REPL (Phase 24.1b): serve the LIVE image over TCP.
 *
 * Deliberately a dedicated, blocking, one-connection-at-a-time server:
 *  - accept() sleeps in the kernel until a peer completes its handshake --
 *    interrupt-driven wake-up, never a poll spin (the house rule). It returns
 *    <0 if a signal lands, which is how Ctrl-C on the console stops the server.
 *  - We do NOT try to multiplex the serial console and the socket with poll():
 *    the kernel's poll treats the console as always-ready, which would turn
 *    the loop into a busy spin. One transport per process, blocking reads.
 *  - The Lisp image (symbol table, global bindings) lives in this process and
 *    is untouched between connections, so state accumulates across visits.
 */
static int serve_repl(int port)
{
    static Writer con;          /* the serial console, for status messages */
    writer_to_fd(&con, 1);

    int srv = socket(SOCK_STREAM);
    if (srv < 0) { w_str(&con, "lisp: socket() failed\n"); return 1; }
    if (bind(srv, (unsigned short)port) < 0) {
        w_str(&con, "lisp: bind() failed (port in use?)\n"); return 1;
    }
    if (listen(srv, 1) < 0) { w_str(&con, "lisp: listen() failed\n"); return 1; }

    w_str(&con, "lisp: serving on port "); w_long(&con, port);
    w_str(&con, " (Ctrl-C to stop)\n");

    for (;;) {
        int conn = accept(srv);            /* blocks; <0 on signal (EINTR) */
        if (conn < 0) {
            if (lm_sigint_pending) { lm_sigint_pending = 0; break; }
            continue;
        }

        /* Point the core's streams at the socket. tty=0: a socket needs no
         * echo or backspace handling -- the editor at the far end does that. */
        static Reader in;
        static Writer out;
        reader_from_fd(&in, conn, 0);
        writer_to_fd(&out, conn);
        lm_cur_in = &in;
        lm_cur_out = &out;

        w_str(&out, BANNER);
        repl_loop(&out);                   /* until the peer disconnects (EOF) */

        sys_close(conn);
        lm_cur_in = 0;
        lm_cur_out = &con;
        w_str(&con, "lisp: connection closed; image retained\n");
    }

    sys_close(srv);
    w_str(&con, "lisp: server stopped\n");
    return 0;
}

/* The interactive REPL on the serial console (tty=1: echo + line editing). */
static int serial_repl(Writer *out)
{
    static Reader in;
    reader_from_fd(&in, 0, 1);
    lm_cur_in = &in;
    repl_loop(out);
    return 0;
}

int umain(int argc, char **argv)
{
    /* Record the high end of the C stack for the conservative collector. umain
     * is the outermost C frame (crt0 calls it), so every frame that can hold a
     * live Lisp object sits below this address. */
    volatile int stack_anchor;
    lm_stack_base = (uintptr_t)&stack_anchor;

    signal(SIGINT, on_sigint);

    lm_boot();
    lm_sys_register();   /* add the syscall primitives (user build only) */

    static Writer out;
    writer_to_fd(&out, 1);
    lm_cur_out = &out;

    /* Load the standard library: bootstrap.l builds the language library,
     * system.l builds the shell on the syscall primitives registered above.
     * (Both placed under /lib by the initrd.) */
    lm_eval_all_str("(load \"/lib/bootstrap.l\")");
    lm_eval_all_str("(load \"/lib/system.l\")");

    if (argc >= 2 && streq(argv[1], "-serve")) {
        int port = 7777;       /* NOT 7000: macOS AirPlay squats on 7000 */
        if (argc >= 3) {
            port = parse_port(argv[2]);
            if (port < 0) { w_str(&out, "lisp: bad port\n"); return 1; }
        }
        return serve_repl(port);
    }

    w_str(&out, BANNER);
    return serial_repl(&out);
}
