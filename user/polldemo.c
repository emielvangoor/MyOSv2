// polldemo.c -- demonstrate poll(): wait on a pipe that a child fills later.
// =========================================================================
//
// The parent creates a pipe and forks. The child waits a moment, then writes to
// the pipe. The parent poll()s the read end with a timeout -- it sleeps until the
// child's write makes the fd readable, then reads it. This is the whole point of
// poll: block on a descriptor becoming ready without busy-waiting.
#include "ulib.h"

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }

int umain(void)
{
    int fd[2];
    if (pipe(fd) < 0) { puts1("polldemo: pipe failed\n"); return 1; }

    long pid = sys_fork();
    if (pid == 0) {                 // child: fill the pipe after a short delay
        sys_sleep(300);
        sys_write(fd[1], "ping", 4);
        sys_exit(0);
    }

    struct pollfd pf;
    pf.fd = fd[0]; pf.events = POLLIN; pf.revents = 0;
    puts1("polldemo: poll() waiting up to 2s for the pipe...\n");
    int r = poll(&pf, 1, 2000);

    if (r > 0 && (pf.revents & POLLIN)) {
        char buf[16];
        int n = (int)sys_read(fd[0], buf, sizeof(buf));
        puts1("polldemo: ready -> read \"");
        sys_write(1, buf, n);
        puts1("\"\n");
    } else {
        puts1("polldemo: timed out (poll returned with nothing ready)\n");
    }

    sys_wait(0);
    return 0;
}
