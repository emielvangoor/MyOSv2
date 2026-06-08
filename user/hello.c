// hello.c -- the /bin/hello coreutil: print a greeting and succeed.
// A real ELF program loaded from /bin and run via fork+exec by the shell.
#include "ulib.h"

int umain(void)
{
    const char *msg = "Hello from /bin/hello\n";
    sys_write(1, msg, ustrlen(msg));
    return 0;
}
