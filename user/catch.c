// catch.c -- /bin/catch: install a SIGINT handler that prints on each Ctrl-C and
// exits after three. Demonstrates user-space signal handlers + sigreturn.
#include "ulib.h"

static volatile int caught = 0;

static void on_int(int sig)
{
    (void)sig;
    sys_write(1, "caught SIGINT\n", 14);
    caught++;
}

int umain(void)
{
    signal(SIGINT, on_int);
    sys_write(1, "catch: press Ctrl-C three times\n", 32);
    for (;;) {
        if (caught >= 3) {
            sys_write(1, "catch: bye\n", 11);
            return 0;
        }
    }
}
