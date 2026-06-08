// mtest.c -- /bin/mtest: a self-test for user-space malloc/free + sbrk.
// Exercises non-overlapping blocks, free+reuse, and a multi-page allocation,
// printing "mtest: ok" (exit 0) or the first failure (exit 1).
#include "ulib.h"

static void say(const char *s) { sys_write(1, s, ustrlen(s)); }

int umain(void)
{
    // 1. Two distinct blocks must not overlap.
    char *a = malloc(32);
    char *b = malloc(32);
    if (!a || !b) { say("mtest: alloc FAIL\n"); return 1; }
    for (int i = 0; i < 32; i++) { a[i] = (char)i; b[i] = (char)(100 + i); }
    for (int i = 0; i < 32; i++) {
        if (a[i] != (char)i || b[i] != (char)(100 + i)) { say("mtest: overlap FAIL\n"); return 1; }
    }

    // 2. A freed block of the same size is reused.
    free(a);
    char *c = malloc(32);
    if (c != a) { say("mtest: reuse FAIL\n"); return 1; }

    // 3. A large allocation forces sbrk to map several pages.
    char *big = malloc(9000);
    if (!big) { say("mtest: big FAIL\n"); return 1; }
    big[0] = 1; big[8999] = 2;
    if (big[0] != 1 || big[8999] != 2) { say("mtest: big rw FAIL\n"); return 1; }

    say("mtest: ok\n");
    return 0;
}
