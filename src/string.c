// string.c -- the freestanding mem* the compiler is allowed to call implicitly.
// ============================================================================
//
// We build with -ffreestanding, but GCC is still permitted to emit calls to
// memset/memcpy/memmove/memcmp for things it does not open-code -- most often
// zero-filling the unmentioned tail of a partial aggregate initializer, e.g.
//
//     struct file wf = { .pipe = p, .ref = 1 };   // rest zeroed via memset
//
// For small structs GCC inlines the stores; once a struct grows past a size
// threshold it switches to a memset CALL instead. The kernel links with
// -nostdlib, so if we do not define these ourselves that call is an undefined
// symbol at link time. Defining them here makes the implicit calls resolve --
// and gives the rest of the kernel a real mem* to use directly.
//
// Plain, portable byte loops: correct and obvious. The compiler is free to
// recognise and optimise them; we are not chasing word-at-a-time speed here.

#include <stddef.h>
#include "string.h"

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) { d[i] = (unsigned char)c; }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) { d[i] = s[i]; }
    return dst;
}

// memmove must handle overlapping ranges: copy back-to-front when dst is above
// src so we never clobber bytes we still need to read.
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) { return dst; }
    if (d < s) {
        for (size_t i = 0; i < n; i++) { d[i] = s[i]; }
    } else {
        for (size_t i = n; i > 0; i--) { d[i - 1] = s[i - 1]; }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) { return (int)p[i] - (int)q[i]; }
    }
    return 0;
}
