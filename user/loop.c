// loop.c -- /bin/loop: spin forever with no signal handler, so Ctrl-C (SIGINT)
// terminates it by the default action. Demonstrates killing a runaway program.
#include "ulib.h"

int umain(void)
{
    sys_write(1, "looping (Ctrl-C to stop)...\n", 27);
    for (;;) { }
    return 0;
}
