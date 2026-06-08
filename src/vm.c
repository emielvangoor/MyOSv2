// vm.c -- per-process address spaces built from PMM pages.
// =======================================================
//
// Each process gets its own top-level table (l0). l0[0] points at the SHARED
// kernel L1 (so the kernel works after a trap, no matter which process is live);
// l0[1] is PRIVATE and maps the user program at fixed user VAs using 4 KiB pages:
//   - code  : shared, read-only, EL0-executable
//   - stack : private, EL0 read/write
//   - data  : private, EL0 read/write
// Isolation comes from each process getting its own physical stack/data pages.
//
// Page-table pages come from the PMM (4 KiB-aligned). Because RAM is
// identity-mapped for the kernel, a physical table address is also a valid
// kernel pointer, so we can build and walk the tables in C directly.

#include <stdint.h>
#include "vm.h"
#include "pmm.h"
#include "mmu.h"

// Descriptor bits (4 KiB granule).
#define DESC_TABLE      (3UL << 0)
#define DESC_PAGE       (3UL << 0)    // at L3, 0b11 = a 4 KiB page
#define ATTR_AF         (1UL << 10)
#define ATTR_SH_INNER   (3UL << 8)
#define ATTR_IDX_NORMAL (1UL << 2)    // MAIR slot 1 = Normal (matches mmu.c)
#define AP_RW_ALL       (1UL << 6)    // EL1+EL0 read/write
#define AP_RO_ALL       (3UL << 6)    // EL1+EL0 read-only
#define ATTR_UXN        (1UL << 54)
#define ATTR_PXN        (1UL << 53)

#define PAGE 4096UL

// The embedded user program (a flat binary linked at USER_CODE_VA), generated
// from user/ by the Makefile (build/user_blob.c).
extern unsigned char init_bin[];
extern unsigned int  init_bin_len;

// Allocate a zeroed page-table page from the PMM.
static uint64_t *alloc_table(void)
{
    uint64_t *t = (uint64_t *)pmm_alloc();
    for (int i = 0; i < 512; i++) {
        t[i] = 0;
    }
    return t;
}

void vm_init(void) { }   // nothing to prepare globally now (code is per-process)

uint64_t user_entry_va(void)
{
    // The program is linked at USER_CODE_VA with its entry (_start, in crt0) at
    // offset 0, so the entry virtual address is simply USER_CODE_VA.
    return USER_CODE_VA;
}

// Map one 4 KiB page (va -> pa with attrs), creating the private L1/L2/L3 tables
// under l0 on demand.
static void map_page(uint64_t *l0, uint64_t va, uint64_t pa, uint64_t attrs)
{
    uint64_t l0i = (va >> 39) & 511;
    uint64_t l1i = (va >> 30) & 511;
    uint64_t l2i = (va >> 21) & 511;
    uint64_t l3i = (va >> 12) & 511;

    if (!(l0[l0i] & 1)) {
        l0[l0i] = (uint64_t)(uintptr_t)alloc_table() | DESC_TABLE;
    }
    uint64_t *l1 = (uint64_t *)(l0[l0i] & ~0xFFFUL);
    if (!(l1[l1i] & 1)) {
        l1[l1i] = (uint64_t)(uintptr_t)alloc_table() | DESC_TABLE;
    }
    uint64_t *l2 = (uint64_t *)(l1[l1i] & ~0xFFFUL);
    if (!(l2[l2i] & 1)) {
        l2[l2i] = (uint64_t)(uintptr_t)alloc_table() | DESC_TABLE;
    }
    uint64_t *l3 = (uint64_t *)(l2[l2i] & ~0xFFFUL);
    l3[l3i] = (pa & ~0xFFFUL) | attrs | DESC_PAGE;
}

// Build an address space whose private code pages hold `img` (len bytes), mapped
// read-only EL0-executable at USER_CODE_VA; with private stack + data pages.
struct addrspace *as_create_image(const void *img, uint64_t len)
{
    struct addrspace *as = (struct addrspace *)pmm_alloc();
    as->l0 = alloc_table();

    // Share the kernel mapping (identity, EL1-only) at L0[0].
    as->l0[0] = mmu_kernel_l0_entry();

    uint64_t code_attr = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RO_ALL;  // RO, EL0-exec
    uint64_t rw_attr   = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                         ATTR_UXN | ATTR_PXN;                                     // RW, no-exec

    // Private code pages: copy the image in, padding the last page with zeros.
    uint64_t npages = (len + PAGE - 1) / PAGE;
    if (npages == 0) { npages = 1; }
    const uint8_t *src = (const uint8_t *)img;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
        uint8_t *dst = (uint8_t *)(uintptr_t)pa;
        for (uint64_t j = 0; j < PAGE; j++) {
            uint64_t k = i * PAGE + j;
            dst[j] = (k < len) ? src[k] : 0;
        }
        map_page(as->l0, USER_CODE_VA + i * PAGE, pa, code_attr);
    }

    // Private stack: 16 pages ending at USER_STACK_TOP, freshly allocated.
    for (uint64_t i = 1; i <= 16; i++) {
        uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
        map_page(as->l0, USER_STACK_TOP - i * PAGE, pa, rw_attr);
    }

    // Private data: one page.
    uint64_t data_pa = (uint64_t)(uintptr_t)pmm_alloc();
    map_page(as->l0, USER_DATA_VA, data_pa, rw_attr);

    return as;
}

// Convenience: build an address space from the embedded user program.
struct addrspace *as_create(void)
{
    return as_create_image(init_bin, (uint64_t)init_bin_len);
}

uint64_t as_translate(struct addrspace *as, uint64_t va)
{
    uint64_t e = as->l0[(va >> 39) & 511];
    if (!(e & 1)) return 0;
    uint64_t *l1 = (uint64_t *)(e & ~0xFFFUL);
    e = l1[(va >> 30) & 511];
    if (!(e & 1)) return 0;
    if ((e & 3) == 1) return (e & ~0x3FFFFFFFUL) | (va & 0x3FFFFFFF);   // 1 GiB block
    uint64_t *l2 = (uint64_t *)(e & ~0xFFFUL);
    e = l2[(va >> 21) & 511];
    if (!(e & 1)) return 0;
    if ((e & 3) == 1) return (e & ~0x1FFFFFUL) | (va & 0x1FFFFF);       // 2 MiB block
    uint64_t *l3 = (uint64_t *)(e & ~0xFFFUL);
    e = l3[(va >> 12) & 511];
    if (!(e & 1)) return 0;
    return (e & ~0xFFFUL) | (va & 0xFFF);                              // 4 KiB page
}

void as_switch(struct addrspace *as)
{
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)as->l0));
    __asm__ volatile("isb");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}
