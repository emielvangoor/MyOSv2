// pmm.h -- Physical Memory Manager: hands out free RAM in 4 KiB pages.
#pragma once
#include <stddef.h>

void  pmm_init(void);            // set up the allocator over free RAM
void *pmm_alloc(void);           // one 4 KiB page (page-aligned), or NULL if out
void *pmm_alloc_pages(size_t n); // n CONTIGUOUS pages, or NULL (used by the heap)
void  pmm_free(void *page);      // return a single page to the allocator
