// fptest.c -- /bin/fptest: prove the FPU works and FP state is per-process.
// ========================================================================
// The FPU phase's acceptance test. Two processes (parent + forked child)
// each park DISTINCT sentinel values directly in V registers, then sleep
// repeatedly -- forcing context switches while the other process is doing
// the same with different values. If the scheduler's fp_save/fp_restore
// pair ever leaked state between threads, a sentinel would come back wrong.
// Plus one honest double computation, because that's the whole point.
#include "ulib.h"

static void put(const char *s) { sys_write(1, s, ustrlen(s)); }
static void putn(long v)
{
    char b[24]; int i = 0;
    if (v < 0) { put("-"); v = -v; }
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

static void wr_v8(unsigned long v)  { __asm__ volatile("dup v8.2d, %0"  :: "r"(v)); }
static void wr_v20(unsigned long v) { __asm__ volatile("dup v20.2d, %0" :: "r"(v)); }
static void wr_v31(unsigned long v) { __asm__ volatile("dup v31.2d, %0" :: "r"(v)); }
static unsigned long rd_v8(void)  { unsigned long r; __asm__ volatile("umov %0, v8.d[0]"  : "=r"(r)); return r; }
static unsigned long rd_v20(void) { unsigned long r; __asm__ volatile("umov %0, v20.d[0]" : "=r"(r)); return r; }
static unsigned long rd_v31(void) { unsigned long r; __asm__ volatile("umov %0, v31.d[0]" : "=r"(r)); return r; }

static int worker(const char *who, unsigned long tag)
{
    wr_v8(tag); wr_v20(tag ^ 0x1234567890ABCDEFUL); wr_v31(tag * 7);
    for (int i = 0; i < 50; i++) {
        sys_sleep(5);                       // force switches mid-flight
        if (rd_v8() != tag ||
            rd_v20() != (tag ^ 0x1234567890ABCDEFUL) ||
            rd_v31() != tag * 7) {
            put("fp: FAIL "); put(who); put("\n");
            return 1;
        }
    }
    put("fp: PASS "); put(who); put("\n");
    return 0;
}

int umain(void)
{
    // Real float math first: pi * 2, scaled to an integer for printing.
    double pi = 3.14159265;
    put("fp: pi*2*100 = "); putn((long)(pi * 2.0 * 100.0)); put("\n");

    long pid = sys_fork();
    if (pid == 0) { sys_exit(worker("child", 0xAAAAAAAA11111111UL)); }
    int rc = worker("parent", 0x5555555522222222UL);
    int st = 0;
    sys_wait(&st);
    return rc + st;
}
