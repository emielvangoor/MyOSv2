// pmm.c -- the Physical Memory Manager (PMM).
// ===========================================
//
// "Physical Memory Manager" = the layer that owns raw RAM and hands it out in
// fixed-size 4 KiB PAGES. It's the coarse, bottom layer; the heap (kheap.c)
// builds finer byte-sized allocation on top of it.
//
// We manage all RAM from the end of the kernel image (__kernel_end, from the
// linker script) up to RAM_END. Two mechanisms:
//   * a BUMP POINTER that sweeps forward through never-yet-used RAM, and
//   * a FREE LIST of pages that were handed back, so they can be reused.
// Because a free page's contents don't matter, we store the "next free page"
// pointer INSIDE the page itself (an intrusive list) -- no extra bookkeeping
// memory needed.

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

#define PAGE_SIZE 4096UL
#define RAM_END   0x50000000UL   // end of RAM; must match the Makefile's -m 256M

// Provided by the linker script: the address just past the kernel image.
extern char __kernel_end[];

static uint64_t bump;        // next never-used page address (the frontier)
static void   *free_list;    // head of the list of returned pages (or NULL)

// Round x up to the next multiple of a (a must be a power of two).
static uint64_t align_up(uint64_t x, uint64_t a)
{
    return (x + a - 1) & ~(a - 1);
}

void pmm_init(void)
{
    bump = align_up((uint64_t)__kernel_end, PAGE_SIZE);
    free_list = NULL;
}

void *pmm_alloc(void)
{
    // Prefer a recycled page: pop the head of the free list. The stored "next"
    // pointer lives in the first 8 bytes of the free page.
    if (free_list) {
        void *page = free_list;
        free_list = *(void **)page;
        return page;
    }
    // Otherwise carve a fresh page off the frontier, if RAM remains.
    if (bump + PAGE_SIZE > RAM_END) {
        return NULL;   // out of physical memory
    }
    void *page = (void *)bump;
    bump += PAGE_SIZE;
    return page;
}

void *pmm_alloc_pages(size_t n)
{
    // A contiguous run must come from the frontier (the free list holds single,
    // possibly non-adjacent pages). Simplification: this ignores the free list.
    uint64_t bytes = (uint64_t)n * PAGE_SIZE;
    if (bump + bytes > RAM_END) {
        return NULL;
    }
    void *run = (void *)bump;
    bump += bytes;
    return run;
}

void pmm_free(void *page)
{
    extern void kprintf(const char *, ...);
    for (void *p = free_list; p; p = *(void **)p) {
        if (p == page) { kprintf("[pmm] DOUBLE FREE %lx\n", (uint64_t)(uintptr_t)page); return; }
    }
    // Push the page onto the free list: store the old head inside this page,
    // then make this page the new head.
    *(void **)page = free_list;
    free_list = page;
}
