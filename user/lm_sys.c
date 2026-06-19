/*
 * lm_sys.c -- the system-call primitives of the Lisp machine (Phase 24.2).
 * =======================================================================
 *
 * This file is what turns the Lisp interpreter into a Lisp *machine*: each
 * primitive below is a DEFUN wrapping one of MyOSv2's syscalls, so Lisp code
 * can fork processes, exec ELF programs, plumb pipes and open sockets -- the
 * raw material the Lisp shell (user/lisp/system.l) is built from.
 *
 * Why this file exists separately from src/lm_core.c: the core is compiled
 * into BOTH the kernel (where the KTEST suite red-greens the language) and
 * /bin/lisp. The kernel build has no syscalls to wrap -- a kernel doesn't
 * fork(2) itself -- so anything that touches ulib must live in a USER-ONLY
 * translation unit. The core stays platform-pure; this file is platform-rich.
 *
 * Because these primitives can't run in the KTEST build, their tests are the
 * boot-and-observe kind: tools/lisp_sys_check.py boots QEMU and exercises
 * every one of them over the network REPL against the real kernel.
 *
 * Conventions, chosen to stay close to both Unix and Lisp:
 *   - fds, pids, ports, statuses are fixnums; paths and data are strings.
 *   - syscall-style results: -1 means failure (no errno yet), so Lisp code
 *     tests (< fd 0) exactly like C code tests fd < 0.
 *   - `fd-read`/`fd-write` (not `read`/`write`): the core already owns `read`
 *     -- it reads a FORM from the current input, the Lisp meaning of the word
 *     -- and shadowing it would break the REPL bootstrap. The fd- prefix says
 *     "this is the file-descriptor, byte-moving flavour".
 */

#include "lm.h"
#include "ulib.h"
#include "lm_sys.h"

/* ---- argument plumbing -------------------------------------------------- */

/* Pull the nth (0-based) argument out of the evaluated arg list. */
static Lobj nth_arg(Lobj args, int n)
{
    while (n-- > 0) { args = CDR(args); }
    return CAR(args);
}

/* Coerce-or-error helpers: primitives fail loudly (through lm_error, which
 * unwinds to the REPL's recovery point) instead of silently misbehaving. */
static int64_t req_fixnum(Lobj o, const char *who)
{
    if (!IS_FIXNUM(o)) { lm_error(who, o); }
    return FIXNUM_VAL(o);
}

static const char *req_string(Lobj o, const char *who)
{
    if (!IS_STRING(o)) { lm_error(who, o); }
    return ((LString *)PTR(o))->data;   /* lm_strdup'd, always NUL-terminated */
}

/* The same DEFUN trick the core uses: define the C function and a one-line
 * registrar that binds it into a symbol's FUNCTION slot (Lisp-2). */
#define DEFSYS(lisp_name, c_name, min, max)                                   \
    static Lobj c_name(Lobj args, Lobj env);                                  \
    static void register_##c_name(void) {                                     \
        Lobj sym = intern(lisp_name);                                         \
        ((Symbol *)PTR(sym))->function = make_prim(lisp_name, c_name, min, max); \
    }                                                                         \
    static Lobj c_name(Lobj args, Lobj env)

/* ---- processes ---------------------------------------------------------- */

/* (getpid) -> pid */
DEFSYS("getpid", Sgetpid, 0, 0) {
    (void)args; (void)env;
    return FIXNUM(sys_getpid());
}

/* (fork) -> 0 in the child, the child's pid in the parent.
 * fork copies the WHOLE process -- including the entire Lisp image -- via the
 * kernel's copy-on-write paths, so both sides resume mid-eval of this very
 * form. That is what makes the Eshell model work: the child can exec an ELF
 * binary while the parent's image is untouched. */
DEFSYS("fork", Sfork, 0, 0) {
    (void)args; (void)env;
    return FIXNUM(sys_fork());
}

/* (exec path argv) -> only returns on failure (-1). argv is a list of strings
 * and must include argv[0] (the program's own name), Unix-style:
 *   (exec "/bin/hello" (list "hello" "world"))                              */
DEFSYS("exec", Sexec, 2, 2) {
    (void)env;
    const char *path = req_string(nth_arg(args, 0), "exec: path must be a string");
    Lobj lst = nth_arg(args, 1);
    char *argv[16];
    int argc = 0;
    for (Lobj p = lst; !IS_NIL(p); p = CDR(p)) {
        if (argc >= 15) { lm_error("exec: too many arguments", lst); }
        argv[argc++] = (char *)req_string(CAR(p), "exec: argv must be strings");
    }
    argv[argc] = 0;
    return FIXNUM(sys_exec(path, argv));   /* reached only when exec failed */
}

/* (wait) -> (pid . status) of a reaped child, or nil if there are none.
 * Returning the pair keeps both halves of the answer; the shell wants the
 * status, a job controller would want the pid. */
DEFSYS("wait", Swait, 0, 0) {
    (void)args; (void)env;
    int status = 0;
    long pid = sys_wait(&status);
    if (pid < 0) { return Qnil; }
    return make_cons(FIXNUM(pid), FIXNUM(status));
}

/* (exit [code]) -> exits the process. EXCEPT when exiting would freeze the
 * machine: init (PID 1) has nothing left to own the terminal or reap orphans,
 * and the graphical frame OWNS the display (exiting it leaves a frozen screen).
 * Both refuse -- the same spirit as serial_repl's EOF guard. Use (shutdown) to
 * actually power off. */
extern int gfx_frame_ready(void);           /* defined in lm_gfx.c (lm.elf only) */
DEFSYS("exit", Sexit, 0, 1) {
    (void)env;
    if (sys_getpid() == 1 || gfx_frame_ready()) {
        // Return the note (don't sys_write): the REPL prints the return value in
        // BOTH the serial REPL and the frame's buffer; a bare sys_write to fd 1
        // would only reach the serial console, invisible in the frame.
        return make_string("exit ignored -- would freeze the machine; use (shutdown) to power off");
    }
    sys_exit(IS_NIL(args) ? 0 : (int)req_fixnum(CAR(args), "exit: code must be a fixnum"));
    return Qnil;                            /* unreachable for init / the frame */
}

/* (kill pid sig) -> 0 or -1. */
DEFSYS("kill", Skill, 2, 2) {
    (void)env;
    int pid = (int)req_fixnum(nth_arg(args, 0), "kill: pid must be a fixnum");
    int sig = (int)req_fixnum(nth_arg(args, 1), "kill: sig must be a fixnum");
    return FIXNUM(kill(pid, sig));
}

/* (setpgid pid pgid) -> 0 or -1. (setpgid 0 0) makes the caller a process-
 * group leader; children it forks inherit the group, so (kill (- 0 pid) 2)
 * later interrupts the WHOLE job -- the frame's C-c story. */
DEFSYS("setpgid", Ssetpgid, 2, 2) {
    (void)env;
    int pid  = (int)req_fixnum(nth_arg(args, 0), "setpgid: pid must be a fixnum");
    int pgid = (int)req_fixnum(nth_arg(args, 1), "setpgid: pgid must be a fixnum");
    return FIXNUM(setpgid(pid, pgid));
}

/* (sleep ms) -> nil, after blocking (the scheduler sleeps us; no spinning). */
DEFSYS("sleep", Ssleep, 1, 1) {
    (void)env;
    sys_sleep(req_fixnum(CAR(args), "sleep: ms must be a fixnum"));
    return Qnil;
}

/* ---- files and pipes ----------------------------------------------------- */

/* (open path) -> fd, or -1. */
DEFSYS("open", Sopen, 1, 1) {
    (void)env;
    return FIXNUM(sys_open(req_string(CAR(args), "open: path must be a string")));
}

/* (creat path) -> fd: open, creating the file if missing. How the machine
 * writes its own configuration: (let ((fd (creat "/init.l"))) ...). */
DEFSYS("creat", Screat, 1, 1) {
    (void)env;
    return FIXNUM(sys_creat(req_string(CAR(args), "creat: path must be a string")));
}

/* (mkdir path) -> t on success or if it already exists, nil on other error. */
DEFSYS("mkdir", Smkdir, 1, 1) {
    (void)env;
    long r = sys_mkdir(req_string(CAR(args), "mkdir: path must be a string"));
    return (r == 0 || r == -17 /* EEXIST */) ? Qt : Qnil;
}

/* (close fd) -> nil. */
DEFSYS("close", Sclose, 1, 1) {
    (void)env;
    sys_close((int)req_fixnum(CAR(args), "close: fd must be a fixnum"));
    return Qnil;
}

/* (fd-read fd n) -> a string of up to n bytes, or nil at EOF.
 * Blocking, like the syscall: on a pipe or socket this sleeps until data
 * arrives. The bytes come back as a Lisp string, so this is for text-ish
 * data -- which is everything this OS's programs speak. */
#define FD_READ_MAX 4096
DEFSYS("fd-read", Sfdread, 2, 2) {
    (void)env;
    int fd = (int)req_fixnum(nth_arg(args, 0), "fd-read: fd must be a fixnum");
    int64_t n = req_fixnum(nth_arg(args, 1), "fd-read: count must be a fixnum");
    if (n < 0) { lm_error("fd-read: negative count", nth_arg(args, 1)); }
    if (n > FD_READ_MAX) { n = FD_READ_MAX; }
    char *buf = lm_alloc((size_t)n + 1);
    long got = sys_read(fd, buf, n);
    if (got <= 0) { lm_free(buf); return Qnil; }
    buf[got] = 0;
    Lobj s = make_string(buf);              /* copies; buf is ours to free */
    lm_free(buf);
    return s;
}

/* (fd-write fd str) -> bytes written, or -1. */
DEFSYS("fd-write", Sfdwrite, 2, 2) {
    (void)env;
    int fd = (int)req_fixnum(nth_arg(args, 0), "fd-write: fd must be a fixnum");
    Lobj s = nth_arg(args, 1);
    if (!IS_STRING(s)) { lm_error("fd-write: data must be a string", s); }
    LString *ls = (LString *)PTR(s);
    return FIXNUM(sys_write(fd, ls->data, (long)ls->len));
}

/* (pipe) -> (read-fd . write-fd), or nil. The cons mirrors the C API's
 * fd[0]/fd[1] but with names: car reads, cdr writes. */
DEFSYS("pipe", Spipe, 0, 0) {
    (void)args; (void)env;
    int fd[2];
    if (pipe(fd) < 0) { return Qnil; }
    return make_cons(FIXNUM(fd[0]), FIXNUM(fd[1]));
}

/* (dup2 oldfd newfd) -> newfd, or -1. The shell's plumbing tool: duplicate
 * a pipe end onto fd 0/1 before exec, and the new program's stdin/stdout
 * point into the pipe without it ever knowing. */
DEFSYS("dup2", Sdup2, 2, 2) {
    (void)env;
    int oldfd = (int)req_fixnum(nth_arg(args, 0), "dup2: oldfd must be a fixnum");
    int newfd = (int)req_fixnum(nth_arg(args, 1), "dup2: newfd must be a fixnum");
    return FIXNUM(dup2(oldfd, newfd));
}

/* (readdir path) -> list of entry-name strings, nil for empty/missing.
 * What (ls) in system.l is made of. Built in reverse and flipped, so the
 * list comes back in the directory's own order. */
DEFSYS("readdir", Sreaddir, 1, 1) {
    (void)env;
    const char *path = req_string(CAR(args), "readdir: path must be a string");
    Lobj names = Qnil;
    char name[32];
    for (int i = 0; sys_readdir(path, i, name) == 0; i++) {
        names = make_cons(make_string(name), names);
    }
    /* reverse in place is overkill here; rebuild forward */
    Lobj out = Qnil;
    for (Lobj p = names; !IS_NIL(p); p = CDR(p)) { out = make_cons(CAR(p), out); }
    return out;
}

/* ---- sockets ------------------------------------------------------------- */

/* (socket 'stream) / (socket 'dgram) -> fd. A symbol, not a magic number:
 * (socket 'stream) reads better than (socket 2) and the symbols are interned
 * so the comparison is pointer equality. */
DEFSYS("socket", Ssocket, 1, 1) {
    (void)env;
    Lobj type = CAR(args);
    if (type == intern("stream")) { return FIXNUM(socket(SOCK_STREAM)); }
    if (type == intern("dgram"))  { return FIXNUM(socket(SOCK_DGRAM)); }
    lm_error("socket: type must be 'stream or 'dgram", type);
    return Qnil;
}

/* (bind fd port) -> 0 or -1. */
DEFSYS("bind", Sbind, 2, 2) {
    (void)env;
    int fd = (int)req_fixnum(nth_arg(args, 0), "bind: fd must be a fixnum");
    int64_t port = req_fixnum(nth_arg(args, 1), "bind: port must be a fixnum");
    if (port < 1 || port > 65535) { lm_error("bind: bad port", nth_arg(args, 1)); }
    return FIXNUM(bind(fd, (unsigned short)port));
}

/* (listen fd) -> 0 or -1. */
DEFSYS("listen", Slisten, 1, 1) {
    (void)env;
    return FIXNUM(listen((int)req_fixnum(CAR(args), "listen: fd must be a fixnum"), 1));
}

/* (accept fd) -> connection fd. Blocks (sleep/wake, not polling) until a peer
 * completes its handshake; returns -1 if a signal interrupts the wait. */
DEFSYS("accept", Saccept, 1, 1) {
    (void)env;
    return FIXNUM(accept((int)req_fixnum(CAR(args), "accept: fd must be a fixnum")));
}

/* (connect fd host port) -> 0 or -1. `host` is a name; the resolver (DNS,
 * Phase 22) turns it into an address, so Lisp never sees raw IPs. */
DEFSYS("connect", Sconnect, 3, 3) {
    (void)env;
    int fd = (int)req_fixnum(nth_arg(args, 0), "connect: fd must be a fixnum");
    const char *host = req_string(nth_arg(args, 1), "connect: host must be a string");
    int64_t port = req_fixnum(nth_arg(args, 2), "connect: port must be a fixnum");
    unsigned int ip = resolve(host);
    if (ip == 0) { return FIXNUM(-1); }
    return FIXNUM(connect(fd, ip, (unsigned short)port));
}

/* ---- the machine itself -------------------------------------------------- */

/* (switch-seat n) -> 0/-1: hand the screen + keyboard to display client n
 * (the VT switch; Ctrl-Alt-Fn does the same from the keyboard). */
DEFSYS("switch-seat", Sswitch_seat, 1, 1) {
    (void)env;
    return FIXNUM(seat_switch((int)req_fixnum(CAR(args), "switch-seat: expected a fixnum")));
}

/* (shutdown) -> never returns: halts the machine via PSCI (QEMU exits).
 * The Lisp shell's way to turn the computer off. */
DEFSYS("shutdown", Sshutdown, 0, 0) {
    (void)args; (void)env;
    shutdown();
    return Qnil;                            /* unreachable */
}

/* ---- registration -------------------------------------------------------- */

/* Called once from umain after lm_boot(): add every syscall primitive to the
 * freshly-booted image, before any Lisp source is loaded. */
void lm_sys_register(void)
{
    register_Sgetpid(); register_Sfork(); register_Sexec(); register_Swait();
    register_Sexit(); register_Skill(); register_Ssetpgid(); register_Ssleep();
    register_Sopen(); register_Screat(); register_Smkdir(); register_Sclose(); register_Sfdread(); register_Sfdwrite();
    register_Spipe(); register_Sdup2(); register_Sreaddir();
    register_Ssocket(); register_Sbind(); register_Slisten(); register_Saccept();
    register_Sconnect();
    register_Sswitch_seat();
    register_Sshutdown();
}
