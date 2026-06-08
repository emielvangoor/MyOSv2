// prog.c -- the user program "init": greet, read a file, print it, exit.
// Runs at EL0; talks to the kernel only through syscalls (ulib).
#include "ulib.h"

void umain(void)
{
    const char *g = "loaded program says hello (running at EL0)\n";
    sys_write(1, g, ustrlen(g));

    long fd = sys_open("/motd");
    if (fd >= 0) {
        char buf[64];
        long n = sys_read(fd, buf, 63);
        if (n > 0) { sys_write(1, buf, n); }
        sys_close(fd);
    }
    sys_exit(0);
}
