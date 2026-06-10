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
#include "elf.h"

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
#define ATTR_NG         (1UL << 11)   // non-global: entry is tagged by the current ASID

#define PAGE 4096UL
#define PA_MASK 0x0000FFFFFFFFF000UL    // physical address bits [47:12] of a descriptor

// ---- Page reference counts (for copy-on-write sharing across address spaces) ----
#define PTE_COW   (1UL << 55)          // software-use bit: "copy on write"
#define PTE_SHARED (1UL << 56)         // software-use bit: "genuinely shared
                                       // memory (shm canvas, framebuffer) --
                                       // fork must NOT copy-on-write this"
#define RAM_BASE  0x40000000UL
#define RAM_TOP   0x50000000UL
#define NPAGES    ((RAM_TOP - RAM_BASE) / PAGE)
static uint16_t page_ref[NPAGES];

// ---- ASIDs: tag TLB entries per address space (Phase 11) ----
// Each address space gets a small ID; the CPU tags its cached translations with
// that ID and only uses entries whose tag matches the current ASID (in
// TTBR0_EL1[63:48]). So a context switch just changes the ASID -- no TLB flush.
#define ASID_MAX 0xFFFF
static uint32_t next_asid = 1;        // 0 is reserved (boot/unused TTBR0 value)

// Freed ASIDs (from destroyed address spaces) are recycled before bumping the
// counter, so a fork/exec/exit-heavy workload doesn't burn through the space.
#define ASID_FREE_MAX 256
static uint16_t asid_free_list[ASID_FREE_MAX];
static int      asid_free_n;

static void flush_all_tlb(void)       // drop EVERY EL1 TLB entry (rollover only)
{
    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

void asid_free(uint16_t a)
{
    if (a != 0 && asid_free_n < ASID_FREE_MAX) { asid_free_list[asid_free_n++] = a; }
}

uint16_t asid_alloc(void)
{
    if (asid_free_n > 0) {            // reuse a freed ASID first
        return asid_free_list[--asid_free_n];
    }
    if (next_asid > ASID_MAX) {       // handed out every ID -> recycle from 1
        next_asid = 1;
        flush_all_tlb();              // clear any dead space's entries under reused IDs
    }
    return (uint16_t)(next_asid++);
}

// An embedded user program (ELF), generated from user/ by the Makefile
// (build/user_blob.c). Used only by the flat test loader as_create()/_image();
// real program loading goes through as_create_elf().
extern unsigned char sh_elf[];
extern unsigned int  sh_elf_len;

static void page_incref(uint64_t pa);   // defined in the COW section below

// Allocate a zeroed page-table page from the PMM.
static uint64_t *alloc_table(void)
{
    uint64_t *t = (uint64_t *)pmm_alloc();
    for (int i = 0; i < 512; i++) {
        t[i] = 0;
    }
    return t;
}

void vm_init(void)
{
    for (uint64_t i = 0; i < NPAGES; i++) { page_ref[i] = 0; }   // reset refcounts
    next_asid = 1;                                               // reset ASID allocator
    asid_free_n = 0;                                             // empty the recycle list
}

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

// Allocate a fresh top-level table that shares the kernel mapping at L0[0].
uint64_t *as_alloc_l0(void)
{
    uint64_t *l0 = alloc_table();
    l0[0] = mmu_kernel_l0_entry();   // shared kernel map (identity, EL1-only)
    return l0;
}

// Map a program segment: the page-aligned range covering [vaddr, vaddr+memsz).
// Each page is freshly allocated and zeroed, then the overlapping slice of
// [src, src+filesz) is copied in -- so the .bss tail beyond filesz stays zero.
void as_map_segment(struct addrspace *as, uint64_t vaddr, const void *src,
                    uint64_t filesz, uint64_t memsz, int writable, int exec)
{
    // Two independent axes: `writable` picks the access permission (RW vs RO),
    // `exec` picks executability (UXN/PXN). A segment can be both writable and
    // executable -- e.g. a tiny program whose .bss got merged with .text into one
    // RWE segment -- so exec must NOT be implied by !writable.
    uint64_t attr = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | ATTR_NG;
    attr |= writable ? AP_RW_ALL : AP_RO_ALL;
    if (!exec) { attr |= ATTR_UXN | ATTR_PXN; }                  // non-exec -> no-exec

    uint64_t vstart = vaddr & ~0xFFFUL;
    uint64_t vend   = (vaddr + memsz + PAGE - 1) & ~0xFFFUL;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t pva = vstart; pva < vend; pva += PAGE) {
        uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
        uint8_t *dst = (uint8_t *)(uintptr_t)pa;
        for (uint64_t i = 0; i < PAGE; i++) {
            uint64_t va = pva + i;
            dst[i] = (va >= vaddr && va < vaddr + filesz) ? s[va - vaddr] : 0;
        }
        map_page(as->l0, pva, pa, attr);
        page_incref(pa);
    }
}

// Build an address space whose private code pages hold `img` (len bytes), mapped
// read-only EL0-executable at USER_CODE_VA; with private stack + data pages.
struct addrspace *as_create_image(const void *img, uint64_t len)
{
    struct addrspace *as = (struct addrspace *)pmm_alloc();
    as->l0 = as_alloc_l0();
    as->asid = asid_alloc();

    // User pages are non-global (ATTR_NG): they're tagged by this space's ASID,
    // so they stay isolated even though we no longer flush on every switch.
    uint64_t code_attr = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RO_ALL | ATTR_NG;  // RO, EL0-exec
    uint64_t rw_attr   = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                         ATTR_UXN | ATTR_PXN | ATTR_NG;                                     // RW, no-exec

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
        page_incref(pa);
    }

    // Private stack: 64 pages (256 KiB) ending at USER_STACK_TOP. 16 pages
    // proved too tight for a recursive-evaluator Lisp under load.
    for (uint64_t i = 1; i <= 64; i++) {
        uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
        map_page(as->l0, USER_STACK_TOP - i * PAGE, pa, rw_attr);
        page_incref(pa);
    }

    // Private data: one page.
    uint64_t data_pa = (uint64_t)(uintptr_t)pmm_alloc();
    map_page(as->l0, USER_DATA_VA, data_pa, rw_attr);
    page_incref(data_pa);

    as->heap_base = USER_HEAP_BASE;
    as->heap_end  = USER_HEAP_BASE;
    as->mmap_next = USER_MMAP_BASE;
    return as;
}

// Convenience: build an address space from the embedded user program.
struct addrspace *as_create(void)
{
    return as_create_image(sh_elf, (uint64_t)sh_elf_len);
}

// Build an address space from an ELF image: map its PT_LOAD segments at their
// own virtual addresses with proper permissions, add a private user stack, and
// return the program's entry point in *entry. Returns 0 if the ELF is invalid.
struct addrspace *as_create_elf(const void *img, uint64_t len, uint64_t *entry)
{
    struct addrspace *as = (struct addrspace *)pmm_alloc();
    as->l0   = as_alloc_l0();
    as->asid = asid_alloc();

    if (elf_load(as, img, len, entry) != 0) {
        as_destroy(as);
        return 0;
    }

    // Private stack: 64 pages (256 KiB) ending at USER_STACK_TOP. 16 pages
    // proved too tight for a recursive-evaluator Lisp under load.
    uint64_t rw = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                  ATTR_UXN | ATTR_PXN | ATTR_NG;
    for (uint64_t i = 1; i <= 64; i++) {
        uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
        map_page(as->l0, USER_STACK_TOP - i * PAGE, pa, rw);
        page_incref(pa);
    }
    as->heap_base = USER_HEAP_BASE;     // empty heap, ready for sbrk
    as->heap_end  = USER_HEAP_BASE;
    as->mmap_next = USER_MMAP_BASE;
    return as;
}

// Grow (or shrink) the per-process heap. Returns the OLD break. Pages newly
// covered by [heap_base, heap_end) are freshly allocated, zeroed, and mapped RW
// (user data). A shrink just lowers the break (pages stay mapped). Classic sbrk.
uint64_t as_sbrk(struct addrspace *as, long incr)
{
    uint64_t old = as->heap_end;
    uint64_t neu = old + (uint64_t)incr;     // signed add (incr may be negative)
    if (incr > 0) {
        uint64_t rw = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                      ATTR_UXN | ATTR_PXN | ATTR_NG;
        uint64_t first = (old + PAGE - 1) & ~0xFFFUL;   // first not-yet-mapped page
        uint64_t last  = (neu + PAGE - 1) & ~0xFFFUL;   // page boundary past the break
        for (uint64_t va = first; va < last; va += PAGE) {
            uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
            if (!pa) { return (uint64_t)-1; }
            uint8_t *p = (uint8_t *)(uintptr_t)pa;
            for (uint64_t i = 0; i < PAGE; i++) { p[i] = 0; }   // demand-zero
            map_page(as->l0, va, pa, rw);
            page_incref(pa);
        }
    }
    as->heap_end = neu;
    return old;
}

// Does the PTE for `va` allow EL0 writes? (Test introspection: COW demotion
// flips this off; shared mappings must keep it on across a fork.)
static uint64_t *pte_ptr(uint64_t *l0, uint64_t va);   // defined below
int as_is_writable(struct addrspace *as, uint64_t va)
{
    uint64_t *pte = pte_ptr(as->l0, va);
    if (!pte || !(*pte & 1)) { return 0; }
    return (*pte & (3UL << 6)) == AP_RW_ALL;
}

uint64_t as_translate(struct addrspace *as, uint64_t va)
{
    uint64_t e = as->l0[(va >> 39) & 511];
    if (!(e & 1)) return 0;
    uint64_t *l1 = (uint64_t *)(e & PA_MASK);
    e = l1[(va >> 30) & 511];
    if (!(e & 1)) return 0;
    if ((e & 3) == 1) return (e & ~0x3FFFFFFFUL) | (va & 0x3FFFFFFF);   // 1 GiB block
    uint64_t *l2 = (uint64_t *)(e & PA_MASK);
    e = l2[(va >> 21) & 511];
    if (!(e & 1)) return 0;
    if ((e & 3) == 1) return (e & ~0x1FFFFFUL) | (va & 0x1FFFFF);       // 2 MiB block
    uint64_t *l3 = (uint64_t *)(e & PA_MASK);
    e = l3[(va >> 12) & 511];
    if (!(e & 1)) return 0;
    return (e & PA_MASK) | (va & 0xFFF);                              // 4 KiB page
}

void as_switch(struct addrspace *as)
{
    // The ASID rides in the top 16 bits of TTBR0, so loading the register sets
    // both the page-table base and the active ASID at once. No TLB flush: the
    // ASID keeps the previous space's (non-global) entries from matching.
    uint64_t ttbr = ((uint64_t)as->asid << 48) | (uint64_t)(uintptr_t)as->l0;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr));
    __asm__ volatile("isb");
}

// ---- Copy-on-write (fork) support ----
//
// A page can be shared by several address spaces after fork. We count
// references so a page is freed only when the last owner drops it. On fork,
// writable user pages are remapped READ-ONLY + COW in both parent and child; a
// write then faults, and cow_fault() makes a private copy.

static uint64_t pidx(uint64_t pa) { return (pa - RAM_BASE) / PAGE; }

int page_refcount(uint64_t pa)
{
    if (pa < RAM_BASE || pa >= RAM_TOP) { return 0; }
    return page_ref[pidx(pa)];
}
static void page_incref(uint64_t pa)
{
    if (pa >= RAM_BASE && pa < RAM_TOP) { page_ref[pidx(pa)]++; }
}
static void page_decref(uint64_t pa)
{
    if (pa < RAM_BASE || pa >= RAM_TOP) { return; }
    if (page_ref[pidx(pa)] > 0) { page_ref[pidx(pa)]--; }
    if (page_ref[pidx(pa)] == 0) { pmm_free((void *)(uintptr_t)pa); }
}

// Return a pointer to the L3 page-table entry for `va` in `l0`, or 0 if any
// level along the way is missing.
static uint64_t *pte_ptr(uint64_t *l0, uint64_t va)
{
    uint64_t e = l0[(va >> 39) & 511];
    if (!(e & 1)) { return 0; }
    uint64_t *l1 = (uint64_t *)(e & PA_MASK);
    e = l1[(va >> 30) & 511];
    if (!(e & 1) || (e & 3) == 1) { return 0; }
    uint64_t *l2 = (uint64_t *)(e & PA_MASK);
    e = l2[(va >> 21) & 511];
    if (!(e & 1) || (e & 3) == 1) { return 0; }
    uint64_t *l3 = (uint64_t *)(e & PA_MASK);
    if (!(l3[(va >> 12) & 511] & 1)) { return 0; }
    return &l3[(va >> 12) & 511];
}

// Public wrapper over the internal walk: the L3 entry pointer for `va`, or 0.
uint64_t *as_pte(struct addrspace *as, uint64_t va)
{
    return pte_ptr(as->l0, va);
}

// Invalidate only THIS address space's TLB entries (by ASID), not the whole TLB.
// `tlbi aside1, Xt` drops entries whose tag equals Xt[63:48].
static void flush_tlb_asid(uint16_t asid)
{
    uint64_t arg = (uint64_t)asid << 48;
    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi aside1, %0" :: "r"(arg));
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

// Clone an address space copy-on-write by walking the parent's entire user
// page-table subtree (l0[1]): so code, stack, data, heap, mmap -- everything --
// is cloned. For each present page:
//   - writable (RW): demote BOTH parent and child to read-only + COW; a later
//     write faults and cow_fault() makes a private copy.
//   - read-only (code, or an already-COW page from an earlier fork): share with
//     the parent's EXACT attributes, so an already-COW page stays COW (keeping
//     the COW bit -- losing it would turn a later write into a fatal fault).
struct addrspace *as_clone(struct addrspace *parent)
{
    struct addrspace *child = (struct addrspace *)pmm_alloc();
    child->l0 = as_alloc_l0();
    // The child inherits the parent's heap break and mmap position (so malloc,
    // sbrk and mmap keep working after fork).
    child->heap_base = parent->heap_base;
    child->heap_end  = parent->heap_end;
    child->mmap_next = parent->mmap_next;
    child->asid = asid_alloc();      // a clone is its own address space -> own ASID

    uint64_t ro_cow = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RO_ALL |
                      ATTR_UXN | ATTR_PXN | PTE_COW | ATTR_NG;

    uint64_t e1 = parent->l0[1];     // the user region
    if (e1 & 1) {
        uint64_t *l1 = (uint64_t *)(e1 & PA_MASK);
        for (uint64_t l1i = 0; l1i < 512; l1i++) {
            if (!(l1[l1i] & 1)) { continue; }
            uint64_t *l2 = (uint64_t *)(l1[l1i] & PA_MASK);
            for (uint64_t l2i = 0; l2i < 512; l2i++) {
                if (!(l2[l2i] & 1)) { continue; }
                uint64_t *l3 = (uint64_t *)(l2[l2i] & PA_MASK);
                for (uint64_t l3i = 0; l3i < 512; l3i++) {
                    uint64_t *ppte = &l3[l3i];
                    if (!(*ppte & 1)) { continue; }
                    uint64_t va = (1UL << 39) | (l1i << 30) | (l2i << 21) | (l3i << 12);
                    uint64_t pa = *ppte & PA_MASK;
                    int writable = (*ppte & (3UL << 6)) == AP_RW_ALL;
                    if (*ppte & PTE_SHARED) {
                        // Shared memory stays shared: same attributes, still
                        // writable, in both address spaces. No COW.
                        map_page(child->l0, va, pa, *ppte & ~PA_MASK);
                    } else if (writable) {
                        *ppte = pa | ro_cow | DESC_PAGE;             // demote parent
                        map_page(child->l0, va, pa, ro_cow);        // child read-only + COW
                    } else {
                        map_page(child->l0, va, pa, *ppte & ~PA_MASK); // share exact attrs
                    }
                    page_incref(pa);
                }
            }
        }
    }
    flush_tlb_asid(parent->asid);   // parent's writable entries were demoted to COW
    return child;
}

// Tear down a process's address space: drop a reference on every user page (so a
// COW-shared page survives in its other owner), free the private page-table
// pages, recycle the ASID, and free the top-level table + the struct itself.
// Only the USER region (l0[1]) is walked -- l0[0] is the SHARED kernel mapping
// and must never be freed.
void as_destroy(struct addrspace *as)
{
    uint64_t e1 = as->l0[1];
    if (e1 & 1) {
        uint64_t *l1 = (uint64_t *)(e1 & PA_MASK);
        for (int i = 0; i < 512; i++) {
            if (!(l1[i] & 1)) { continue; }
            uint64_t *l2 = (uint64_t *)(l1[i] & PA_MASK);
            for (int j = 0; j < 512; j++) {
                if (!(l2[j] & 1)) { continue; }
                uint64_t *l3 = (uint64_t *)(l2[j] & PA_MASK);
                for (int k = 0; k < 512; k++) {
                    if (l3[k] & 1) { page_decref(l3[k] & PA_MASK); }
                }
                pmm_free(l3);
            }
            pmm_free(l2);
        }
        pmm_free(l1);
    }
    pmm_free(as->l0);
    // Invalidate this ASID's TLB entries BEFORE recycling it: otherwise a future
    // address space that reuses the ASID would hit this dead one's stale entries.
    flush_tlb_asid(as->asid);
    asid_free(as->asid);
    pmm_free(as);
}

// Handle a write to a COW page: make a private copy and remap it read/write.
// Returns 1 if it handled a COW fault, 0 if `va` isn't a COW page (a real fault).
int cow_fault(struct addrspace *as, uint64_t va)
{
    va &= ~0xFFFUL;
    uint64_t *pte = pte_ptr(as->l0, va);
    if (!pte || !(*pte & PTE_COW)) { return 0; }

    uint64_t oldpa = *pte & PA_MASK;
    uint64_t rw = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                  ATTR_UXN | ATTR_PXN | ATTR_NG;

    // Sole owner? No copy needed -- just make the existing page writable.
    if (page_refcount(oldpa) == 1) {
        *pte = (oldpa & ~0xFFFUL) | rw | (3UL << 0);
        flush_tlb_asid(as->asid);
        return 1;
    }

    // Shared: copy to a fresh page (which we now OWN, so take a reference on it),
    // remap it read/write, and drop our reference to the old shared page.
    uint64_t newpa = (uint64_t)(uintptr_t)pmm_alloc();
    uint8_t *dst = (uint8_t *)(uintptr_t)newpa;
    uint8_t *src = (uint8_t *)(uintptr_t)oldpa;
    for (uint64_t i = 0; i < PAGE; i++) { dst[i] = src[i]; }

    *pte = (newpa & ~0xFFFUL) | rw | (3UL << 0);   // page descriptor, RW, no COW
    page_incref(newpa);                            // the faulting space owns the copy
    page_decref(oldpa);                            // ...and no longer the original
    flush_tlb_asid(as->asid);
    return 1;
}

// ---- mmap + shared memory (Phase 16) ----
//
// A simple bump allocator hands out virtual addresses from USER_MMAP_BASE upward.
// Anonymous mmap allocates fresh zeroed pages; as_map_phys maps a caller-supplied
// set of physical pages (used by shared memory, which shares the same pages
// across address spaces).

// Public ++ on a page's refcount (the shm table holds its own reference).
void page_incref_pub(uint64_t pa) { page_incref(pa); }

// Map an anonymous, demand-zeroed, read/write region of `len` bytes; return its
// base VA (or 0 on out-of-memory).
uint64_t as_mmap(struct addrspace *as, uint64_t len)
{
    if (len == 0) { return 0; }
    uint64_t pages = (len + PAGE - 1) / PAGE;
    uint64_t base = as->mmap_next;
    uint64_t rw = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                  ATTR_UXN | ATTR_PXN | ATTR_NG;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t pa = (uint64_t)(uintptr_t)pmm_alloc();
        if (!pa) { return 0; }
        uint8_t *p = (uint8_t *)(uintptr_t)pa;
        for (uint64_t j = 0; j < PAGE; j++) { p[j] = 0; }
        map_page(as->l0, base + i * PAGE, pa, rw);
        page_incref(pa);
    }
    as->mmap_next = base + pages * PAGE;
    return base;
}

// Unmap a region: drop each page's mapping and reference.
int as_munmap(struct addrspace *as, uint64_t va, uint64_t len)
{
    uint64_t pages = (len + PAGE - 1) / PAGE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t *pte = pte_ptr(as->l0, va + i * PAGE);
        if (pte && (*pte & 1)) {
            page_decref(*pte & PA_MASK);
            *pte = 0;
        }
    }
    flush_tlb_asid(as->asid);
    return 0;
}

// Map `n` caller-supplied physical pages read/write at the next mmap VA, bumping
// each page's refcount. Returns the base VA. (Shared memory uses this so several
// address spaces map the SAME physical pages.)
uint64_t as_map_phys(struct addrspace *as, const uint64_t *pa, uint64_t n)
{
    uint64_t base = as->mmap_next;
    // PTE_SHARED: these pages ARE the shared thing (an shm object's pages, the
    // display framebuffer). A fork must keep them writable and shared in both
    // parent and child -- COW-copying them would silently disconnect a process
    // from the very memory it is sharing (found live: after (spawn-vm), the
    // parent VM rendered into a private COW copy of its framebuffer while the
    // GPU kept scanning the original -- a frozen screen).
    uint64_t rw = ATTR_AF | ATTR_SH_INNER | ATTR_IDX_NORMAL | AP_RW_ALL |
                  ATTR_UXN | ATTR_PXN | ATTR_NG | PTE_SHARED;
    for (uint64_t i = 0; i < n; i++) {
        map_page(as->l0, base + i * PAGE, pa[i], rw);
        page_incref(pa[i]);
    }
    as->mmap_next = base + n * PAGE;
    return base;
}
