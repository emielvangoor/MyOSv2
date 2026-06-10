// evtest.c -- /bin/evtest: print input events until Ctrl-C.
// =========================================================
// The Phase 25.1 demo + integration probe: every keyboard/tablet event the
// kernel delivers prints as one "EV <type> <code> <value>" line, so a human
// (or tools/input_check.py) can see exactly what arrived. The read blocks in
// the kernel (sleep/wake on the input interrupt) -- no polling.
#include "ulib.h"

static void put(const char *s) { sys_write(1, s, ustrlen(s)); }
static void putn(long v)
{
    char b[16]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

int umain(void)
{
    put("evtest: waiting for events (Ctrl-C to stop)\n");
    struct input_event ev;
    while (input_read(&ev) == 0) {           // -1 = a signal interrupted us
        put("EV "); putn(ev.type); put(" "); putn(ev.code);
        put(" "); putn(ev.value); put("\n");
    }
    return 0;
}
