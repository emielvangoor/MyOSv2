// wc.c -- /bin/wc: count bytes on stdin and print the total. A real stdin->stdout
// filter, used to demonstrate shell pipelines (`hello | wc`).
#include "ulib.h"

int umain(void)
{
    char buf[256];
    long total = 0, n;
    while ((n = sys_read(0, buf, sizeof(buf))) > 0) { total += n; }

    // print the count as a decimal number + newline
    char b[24];
    int i = 0;
    long v = total;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
    sys_write(1, "\n", 1);
    return 0;
}
