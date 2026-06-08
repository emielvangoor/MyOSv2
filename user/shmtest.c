// shmtest.c -- /bin/shmtest: demonstrate shared memory between two processes.
// The parent creates a shared object; a forked child maps it and writes a
// sentinel; the parent then maps it and reads the sentinel back. Both map AFTER
// the fork, so the mappings are fresh read/write (not copy-on-write).
#include "ulib.h"

static void say(const char *s) { sys_write(1, s, ustrlen(s)); }

int umain(void)
{
    int h = shm_create(4096);
    if (h < 0) { say("shmtest: create FAIL\n"); return 1; }

    long pid = sys_fork();
    if (pid == 0) {                       // child: map and write
        volatile char *p = shm_map(h);
        if (!p) { sys_exit(2); }
        p[0] = 42;
        sys_exit(0);
    }

    int st = 0;
    sys_wait(&st);                        // wait for the child to write

    volatile char *p = shm_map(h);        // parent maps the same object
    if (p && p[0] == 42) { say("shmtest: ok\n"); return 0; }
    say("shmtest: shared FAIL\n");
    return 1;
}
