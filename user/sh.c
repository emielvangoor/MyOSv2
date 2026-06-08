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

static void cmd_ls(void)
{
    char name[32];
    for (int i = 0; sys_readdir("/", i, name) == 0; i++) {
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

// fork a child, exec /bin/<cmd> in it, and wait for it. With no /bin programs yet
// (they arrive in Phase 14) exec fails and the child exits 127 -- but this drives
// the full fork -> exec -> exit -> wait -> reap path the Unix way.
static void run_external(const char *cmd)
{
    char path[64];
    const char *pre = "/bin/";
    int i = 0;
    while (pre[i]) { path[i] = pre[i]; i++; }
    int j = 0;
    while (cmd[j] && i < 62) { path[i++] = cmd[j++]; }
    path[i] = 0;

    long pid = sys_fork();
    if (pid == 0) {                 // child
        sys_exec(path);             // returns only if exec failed
        puts1("exec: not found\n");
        sys_exit(127);
    }
    int st = 0;
    sys_wait(&st);                  // parent reaps the child
    puts1("[exit "); put_int(st); puts1("]\n");
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

int umain(void)
{
    puts1("MyOSv2 shell. Type 'help'.\n");
    char line[128];
    for (;;) {
        puts1("$ ");
        readline(line, sizeof(line));

        // split into command + argument (first space)
        char *arg = line;
        while (*arg && *arg != ' ') { arg++; }
        char *cmd = line;
        if (*arg == ' ') { *arg = 0; arg++; } else { arg = (char *)""; }

        if (cmd[0] == 0)            { continue; }
        else if (streq(cmd, "help")) { puts1("commands: help echo ls cat <f> spawn exit; others run /bin/<cmd>\n"); }
        else if (streq(cmd, "echo")) { puts1(arg); puts1("\n"); }
        else if (streq(cmd, "ls"))   { cmd_ls(); }
        else if (streq(cmd, "cat"))  { cmd_cat(arg); }
        else if (streq(cmd, "spawn")){ cmd_spawn(); }
        else if (streq(cmd, "exit")) { sys_exit(0); }
        else                         { run_external(cmd); }   // fork + exec + wait
    }
}
