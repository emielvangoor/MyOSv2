#pragma once
typedef unsigned long size_t;
void *malloc(unsigned long n);          // ulib
void  free(void *p);                    // ulib
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  exit(int code);
#ifndef NULL
#define NULL ((void *)0)
#endif
