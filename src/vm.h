// vm.h -- per-process address spaces (each user process has its own page tables).
#pragma once
#include <stdint.h>

// User virtual addresses (all in one 2 MiB region under a private L0 entry, so a
// single L3 table covers code + stack + data).
#define USER_CODE_VA   0x8000000000UL   // shared, read-only, EL0-executable
#define USER_STACK_TOP 0x8000100000UL   // private stack grows down from here
#define USER_DATA_VA   0x8000180000UL   // one private data page (flat loader only)
#define USER_HEAP_BASE 0x8000200000UL   // heap grows up from here (sbrk)
#define USER_MMAP_BASE 0x8000400000UL   // mmap + shm mappings, bump up from here

struct addrspace {
    uint64_t *l0;        // top-level table (a PMM page); load into TTBR0_EL1
    uint16_t  asid;      // address-space ID; tags this space's TLB entries
    uint64_t  heap_base; // fixed bottom of the heap (== USER_HEAP_BASE)
    uint64_t  heap_end;  // current program break (grown by sbrk)
    uint64_t  mmap_next; // next free VA for mmap/shm mappings (bump allocator)
};

void vm_init(void);                              // (no-op; kept for callers)
struct addrspace *as_create(void);               // address space from the embedded program
struct addrspace *as_create_image(const void *img, uint64_t len); // from a loaded image (flat)

// User heap (Phase 15): grow the per-process heap on demand; returns old break.
uint64_t as_sbrk(struct addrspace *as, long incr);

// mmap + shared memory (Phase 16).
uint64_t as_mmap(struct addrspace *as, uint64_t len);                 // anon RW -> base VA (0 fail)
int      as_munmap(struct addrspace *as, uint64_t va, uint64_t len);  // unmap + free
uint64_t as_map_phys(struct addrspace *as, const uint64_t *pa, uint64_t n); // map given phys pages
void     page_incref_pub(uint64_t pa);                                // public ++ (for shm table ref)

// ELF program loading (Phase 14).
uint64_t *as_alloc_l0(void);                      // fresh top table sharing the kernel map
void as_map_segment(struct addrspace *as, uint64_t vaddr, const void *src,
                    uint64_t filesz, uint64_t memsz, int writable, int exec);
struct addrspace *as_create_elf(const void *img, uint64_t len, uint64_t *entry); // build from ELF
uint64_t as_translate(struct addrspace *as, uint64_t va); // software walk -> PA (0 if unmapped)
void as_switch(struct addrspace *as);            // TTBR0 = as->l0; flush TLB
uint64_t user_entry_va(void);                    // entry VA of a loaded program

// Fork support: copy-on-write address-space cloning.
struct addrspace *as_clone(struct addrspace *parent);   // COW-share parent's pages
int      cow_fault(struct addrspace *as, uint64_t va);  // copy a COW page on write; 1=handled
int      page_refcount(uint64_t pa);                    // shared-page reference count

// ASID support (Phase 11): tag TLB entries per address space.
uint16_t  asid_alloc(void);                              // next ASID (recycled, then 1..ASID_MAX)
void      asid_free(uint16_t a);                         // recycle a freed ASID (Phase 13)
uint64_t *as_pte(struct addrspace *as, uint64_t va);     // L3 entry pointer for va (0 if unmapped)

// Process teardown (Phase 13): free a process's user pages, page tables, and ASID.
void      as_destroy(struct addrspace *as);
