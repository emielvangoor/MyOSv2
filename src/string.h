// string.h -- prototypes for the freestanding mem* in string.c.
// =============================================================
// Declared so callers (and the compiler, when it emits implicit calls) agree on
// the standard signatures. See string.c for why a -nostdlib kernel defines them.
#pragma once
#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
