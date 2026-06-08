// shm.c -- shared-memory objects.
// ===============================
//
// A shared-memory object owns a set of physical pages. shm_map() installs those
// SAME pages into a caller's address space, so two processes that map the same
// handle see each other's writes. The object holds its own reference on each
// page (so the pages persist even when no process currently maps them); each
// mapping adds another reference, and a process exit (as_destroy) drops it.

#include <stdint.h>
#include "shm.h"
#include "vm.h"
#include "pmm.h"

#define SHM_MAX       16    // max simultaneous objects
#define SHM_PAGES_MAX 16    // max pages (64 KiB) per object

struct shm_obj {
    int      used;
    uint64_t npages;
    uint64_t pa[SHM_PAGES_MAX];
};

static struct shm_obj shms[SHM_MAX];

void shm_init(void)
{
    for (int i = 0; i < SHM_MAX; i++) { shms[i].used = 0; }
}

int shm_create(uint64_t len)
{
    uint64_t pages = (len + 4095) / 4096;
    if (pages == 0 || pages > SHM_PAGES_MAX) { return -1; }

    for (int i = 0; i < SHM_MAX; i++) {
        if (shms[i].used) { continue; }
        shms[i].used   = 1;
        shms[i].npages = pages;
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
            uint8_t *z = (uint8_t *)(uintptr_t)pa;
            for (int b = 0; b < 4096; b++) { z[b] = 0; }
            shms[i].pa[p] = pa;
            page_incref_pub(pa);          // the object's own reference
        }
        return i;
    }
    return -1;   // table full
}

uint64_t shm_map(struct addrspace *as, int handle)
{
    if (handle < 0 || handle >= SHM_MAX || !shms[handle].used) { return 0; }
    return as_map_phys(as, shms[handle].pa, shms[handle].npages);
}
