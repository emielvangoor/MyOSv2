// prog.c -- the "init" program: demonstrates fork + copy-on-write.
// Parent and child each write a different value to the SAME variable; COW gives
// them private copies, so each reads back its own value.
#include "ulib.h"

void umain(void)
{
    const char *intro = "init: forking (testing copy-on-write)\n";
    sys_write(1, intro, ustrlen(intro));

    int x = 0;                       // a variable on the (shared-then-COW) stack
    long pid = sys_fork();

    if (pid == 0) {
        x = 2;                       // child write -> COW copies the page
        char msg[16] = "child:  x=?\n";
        msg[10] = '0' + x;
        sys_write(1, msg, ustrlen(msg));
        sys_exit(0);
    } else {
        x = 1;                       // parent write -> its own copy
        char msg[16] = "parent: x=?\n";
        msg[10] = '0' + x;
        sys_write(1, msg, ustrlen(msg));
        sys_exit(0);
    }
}
