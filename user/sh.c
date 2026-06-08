// sh.c -- a tiny interactive shell (the /bin/init program). Reads a command line
// from the keyboard and runs built-in commands. Talks to the kernel only via
// syscalls (ulib).
#include "ulib.h"

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Read a line into buf (echoing). Returns the length. Handles backspace + enter.
static int readline(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = sys_getc();
        if (c < 0) { continue; }
        if (c == '\r' || c == '\n') { sys_write(1, "\n", 1); buf[n] = 0; return n; }
        if (c == 0x7f || c == 0x08) {                 // backspace
            if (n > 0) { n--; sys_write(1, "\b \b", 3); }
            continue;
        }
        if (n < max - 1) { buf[n++] = (char)c; sys_write(1, (char *)&c, 1); }
    }
}

static void cmd_ls(const char *path)
{
    if (!path || path[0] == 0) { path = "/"; }
    char name[32];
    for (int i = 0; sys_readdir(path, i, name) == 0; i++) {
        puts1(name); puts1("\n");
    }
}

static void cmd_cat(const char *path)
{
    long fd = sys_open(path);
    if (fd < 0) { puts1("cat: no such file\n"); return; }
    char buf[64];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) { sys_write(1, buf, n); }
    sys_close(fd);
}

// Print a small non-negative integer (no trailing newline).
static void put_int(long v)
{
    char b[16];
    int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

// Build "/bin/<cmd>" into dst.
static void bin_path(char *dst, const char *cmd)
{
    const char *pre = "/bin/";
    int i = 0;
    while (pre[i]) { dst[i] = pre[i]; i++; }
    int j = 0;
    while (cmd[j] && i < 62) { dst[i++] = cmd[j++]; }
    dst[i] = 0;
}

// Split `line` in place on spaces into a NULL-terminated argv. Returns argc.
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ') { p++; }
        if (!*p) { break; }
        argv[argc++] = p;
        while (*p && *p != ' ') { p++; }
        if (*p) { *p = 0; p++; }
    }
    argv[argc] = 0;
    return argc;
}

// In a freshly-forked child: exec /bin/<argv[0]> with argv, or report + exit 127
// if not found. argv must be NULL-terminated.
static void child_exec(char *const argv[])
{
    char path[64];
    bin_path(path, argv[0]);
    sys_exec(path, argv);           // returns only if exec failed
    puts1("exec: not found\n");
    sys_exit(127);
}

// fork a child, exec /bin/<cmd> with its arguments, and wait for it. `cmd` is the
// command word; `arg` is the (possibly multi-token, space-separated) remainder,
// which is tokenised in place into the rest of argv.
static void run_external(const char *cmd, char *arg)
{
    char *argv[16];
    int argc = 0;
    argv[argc++] = (char *)cmd;
    char *p = arg;
    while (*p && argc < 15) {
        while (*p == ' ') { p++; }
        if (!*p) { break; }
        argv[argc++] = p;
        while (*p && *p != ' ') { p++; }
        if (*p) { *p = 0; p++; }
    }
    argv[argc] = 0;

    long pid = sys_fork();
    if (pid == 0) { child_exec(argv); }
    int st = 0;
    sys_wait(&st);                  // parent reaps the child
    puts1("[exit "); put_int(st); puts1("]\n");
}

// Run a two-stage pipeline: `left | right`. left's stdout is connected to
// right's stdin through a pipe. The parent closes both pipe ends (so right sees
// EOF once left finishes) and waits for both children.
static void run_pipeline(char *left, char *right)
{
    int fd[2];
    if (pipe(fd) < 0) { puts1("pipe: failed\n"); return; }

    char *lv[16], *rv[16];           // each side gets its own argv (args + all)
    tokenize(left, lv, 16);
    tokenize(right, rv, 16);
    if (!lv[0] || !rv[0]) { sys_close(fd[0]); sys_close(fd[1]); return; }
    long c1 = sys_fork();
    if (c1 == 0) {                  // left: stdout -> pipe write end
        dup2(fd[1], 1);
        sys_close(fd[0]); sys_close(fd[1]);
        child_exec(lv);
    }
    long c2 = sys_fork();
    if (c2 == 0) {                  // right: stdin -> pipe read end
        dup2(fd[0], 0);
        sys_close(fd[0]); sys_close(fd[1]);
        child_exec(rv);
    }
    sys_close(fd[0]); sys_close(fd[1]);   // parent holds neither end
    int st;
    sys_wait(&st);
    sys_wait(&st);
}

// Demonstrate the exit-status path: a child exits with a fixed code; the parent
// waits and prints it.
static void cmd_spawn(void)
{
    long pid = sys_fork();
    if (pid == 0) {
        puts1("  [child] running, exiting with status 3\n");
        sys_exit(3);
    }
    int st = -1;
    long reaped = sys_wait(&st);
    puts1("  [parent] reaped pid "); put_int(reaped); puts1("\n");
    puts1("  [parent] child status "); put_int(st); puts1("\n");
}

// Strip leading and trailing spaces in place; returns the trimmed start.
static char *trim(char *s)
{
    while (*s == ' ') { s++; }
    char *end = s;
    while (*end) { end++; }
    while (end > s && end[-1] == ' ') { *--end = 0; }
    return s;
}

int umain(void)
{
    puts1("MyOSv2 shell. Type 'help'.\n");
    char line[128];
    for (;;) {
        puts1("$ ");
        readline(line, sizeof(line));

        // Pipeline? `left | right` -> connect left's stdout to right's stdin.
        char *bar = line;
        while (*bar && *bar != '|') { bar++; }
        if (*bar == '|') {
            *bar = 0;
            char *left = trim(line);
            char *right = trim(bar + 1);
            if (left[0] && right[0]) { run_pipeline(left, right); }
            continue;
        }

        // split into command + argument (first space)
        char *arg = line;
        while (*arg && *arg != ' ') { arg++; }
        char *cmd = line;
        if (*arg == ' ') { *arg = 0; arg++; } else { arg = (char *)""; }

        if (cmd[0] == 0)            { continue; }
        else if (streq(cmd, "help")) { puts1("commands: help echo ls cat <f> spawn exit shutdown; others run /bin/<cmd>\n"); }
        else if (streq(cmd, "echo")) { puts1(arg); puts1("\n"); }
        else if (streq(cmd, "ls"))   { cmd_ls(arg); }
        else if (streq(cmd, "cat"))  { cmd_cat(arg); }
        else if (streq(cmd, "spawn")){ cmd_spawn(); }
        else if (streq(cmd, "exit")) { sys_exit(0); }
        else if (streq(cmd, "shutdown")) { puts1("Shutting down.\n"); shutdown(); }
        else                         { run_external(cmd, arg); }   // fork + exec + wait
    }
}
