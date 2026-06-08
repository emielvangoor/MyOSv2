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

void umain(void)
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
        else if (streq(cmd, "help")) { puts1("commands: help echo ls cat <f> exit\n"); }
        else if (streq(cmd, "echo")) { puts1(arg); puts1("\n"); }
        else if (streq(cmd, "ls"))   { cmd_ls(); }
        else if (streq(cmd, "cat"))  { cmd_cat(arg); }
        else if (streq(cmd, "exit")) { sys_exit(0); }
        else                         { puts1("unknown command: "); puts1(cmd); puts1("\n"); }
    }
}
