// freestanding shim for TinyGL (see tgl_rt.c)
#pragma once
typedef unsigned long size_t;
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   strcmp(const char *a, const char *b);
size_t strlen(const char *s);
#ifndef NULL
#define NULL ((void *)0)
#endif
int memcmp(const void *a, const void *b, size_t n);
