// ping.c -- /bin/ping: ICMP-echo the QEMU gateway (10.0.2.2) and print the
// round-trip time. (No argv yet, so the address is fixed.)
#include "ulib.h"

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }
static void put_int(int v)
{
    char b[16]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

int umain(void)
{
    int ms = -1;
    int r = ping(0x0a000202u, &ms);     // 10.0.2.2
    if (r == 0) {
        puts1("ping 10.0.2.2: reply in ");
        put_int(ms);
        puts1(" ms\n");
        return 0;
    }
    puts1("ping 10.0.2.2: no reply\n");
    return 1;
}
