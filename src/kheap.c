// kheap.c -- a first-fit free-list heap (kmalloc / kfree).
// =======================================================
//
// The heap serves arbitrary byte-sized requests. It keeps every chunk of memory
// in one doubly-linked list, ORDERED BY ADDRESS. Each chunk has a small header
// followed by its payload:
//
//   [ struct block | payload .... ][ struct block | payload ... ] ...
//
// kmalloc finds a free block big enough (first fit), optionally SPLITS off the
// leftover, and returns a pointer just past the header. kfree marks a block free
// and COALESCES (merges) it with adjacent free neighbors so the space can be
// reused for larger requests later. When nothing fits, the heap GROWS by asking
// the PMM for more pages.

#include <stdint.h>
#include <stddef.h>
#include "kheap.h"
#include "pmm.h"

#define PAGE_SIZE 4096UL

// Round a request up to 8 bytes so every returned pointer is 8-byte aligned.
#define ALIGN8(x) (((x) + 7) & ~((size_t)7))

// Per-chunk header. `size` is the PAYLOAD size (not counting this header).
struct block {
    size_t        size;   // usable bytes after the header
    int           free;   // 1 = available, 0 = in use
    struct block *next;   // next chunk in address order
    struct block *prev;   // previous chunk in address order
};

static struct block *head;   // first chunk in the heap (NULL when empty)

void kheap_init(void)
{
    head = NULL;   // the first kmalloc will grow the heap from nothing
}

// Grow the heap so it can satisfy a payload of `need` bytes: ask the PMM for
// enough whole pages, wrap them as one big free block, and append it to the
// list. Returns the new block, or NULL if the PMM is out of memory.
static struct block *grow_heap(size_t need)
{
    size_t total = sizeof(struct block) + need;
    size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;   // round up to whole pages

    struct block *b = (struct block *)pmm_alloc_pages(pages);
    if (!b) {
        return NULL;
    }

    b->size = pages * PAGE_SIZE - sizeof(struct block);
    b->free = 1;
    b->next = NULL;
    b->prev = NULL;

    if (!head) {
        head = b;
    } else {
        struct block *t = head;
        while (t->next) {
            t = t->next;
        }
        t->next = b;
        b->prev = t;
    }
    return b;
}

// If block `b` is bigger than `size` by at least a header plus a usable minimum,
// carve the tail into a separate free block so we don't waste the leftover.
static void split_block(struct block *b, size_t size)
{
    if (b->size < size + sizeof(struct block) + 8) {
        return;   // leftover too small to be worth its own header
    }
    struct block *rest =
        (struct block *)((uint8_t *)b + sizeof(struct block) + size);
    rest->size = b->size - size - sizeof(struct block);
    rest->free = 1;
    rest->next = b->next;
    rest->prev = b;
    if (b->next) {
        b->next->prev = rest;
    }
    b->next = rest;
    b->size = size;
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    size = ALIGN8(size);

    // First fit: walk the list for a free block large enough.
    for (struct block *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            return (uint8_t *)b + sizeof(struct block);
        }
    }

    // Nothing fit -- grow the heap, then carve the request from the new block.
    struct block *b = grow_heap(size);
    if (!b) {
        return NULL;
    }
    split_block(b, size);
    b->free = 0;
    return (uint8_t *)b + sizeof(struct block);
}

// True if block `a` sits immediately before block `b` in memory (so the two can
// be merged into one contiguous chunk).
static int adjacent(struct block *a, struct block *b)
{
    return (uint8_t *)a + sizeof(struct block) + a->size == (uint8_t *)b;
}

void kfree(void *ptr)
{
    if (!ptr) {
        return;
    }
    // Recover the header that sits just before the payload.
    struct block *b = (struct block *)((uint8_t *)ptr - sizeof(struct block));
    b->free = 1;

    // Merge with the NEXT block if it's free and physically adjacent.
    if (b->next && b->next->free && adjacent(b, b->next)) {
        struct block *n = b->next;
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (n->next) {
            n->next->prev = b;
        }
    }
    // Merge with the PREVIOUS block if it's free and physically adjacent.
    if (b->prev && b->prev->free && adjacent(b->prev, b)) {
        struct block *p = b->prev;
        p->size += sizeof(struct block) + b->size;
        p->next = b->next;
        if (b->next) {
            b->next->prev = p;
        }
    }
}
