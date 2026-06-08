// kheap.h -- the kernel heap: byte-granular kmalloc/kfree on top of the PMM.
#pragma once
#include <stddef.h>

void  kheap_init(void);       // start with an empty heap
void *kmalloc(size_t size);   // allocate `size` bytes (8-byte aligned), or NULL
void  kfree(void *ptr);       // free a kmalloc'd pointer (ignores NULL)
