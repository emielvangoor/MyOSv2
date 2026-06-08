// mmu.c -- build page tables and enable virtual memory (the MMU).
// ==============================================================
//
// With the MMU on, the CPU no longer uses addresses directly. Instead it treats
// every address as VIRTUAL and translates it to a PHYSICAL address by walking a
// tree of "page tables" we set up. We control that tree, so we control what
// every address means.
//
// We use:
//   * 4 KiB pages (the granule), 48-bit virtual addresses.
//   * 2 MiB "block" mappings, so one table entry covers 2 MiB at once -- far
//     fewer entries than mapping individual 4 KiB pages.
//
// Translation walks a 4-level tree (L0 -> L1 -> L2 -> L3). A 2 MiB block stops
// the walk early at L2. A virtual address is sliced into table indices:
//
//   bits 47..39 = L0 index    (each L0 entry spans 512 GiB)
//   bits 38..30 = L1 index    (each L1 entry spans   1 GiB)
//   bits 29..21 = L2 index    (each L2 entry spans   2 MiB)  <- our blocks
//   bits 20..0  = offset within the 2 MiB block
//
// STRATEGY: first IDENTITY-map everything we use (virtual == physical) so that
// flipping the MMU on changes nothing visible. Then add ONE different mapping
// to prove translation actually happens.

#include <stdint.h>
#include "mmu.h"

// ---- Descriptor low bits (what KIND of entry this is) ----
// Every 64-bit table entry's low 2 bits say what it is:
#define DESC_TABLE      (3UL << 0)   // 0b11: points to the next-level table
#define DESC_BLOCK      (1UL << 0)   // 0b01: a block mapping (the leaf)

// ---- Block attribute bits (properties of the mapped memory) ----
#define ATTR_IDX(n)     ((uint64_t)(n) << 2)   // which MAIR slot describes the type
#define ATTR_NS         (0UL << 5)             // non-secure
#define ATTR_AP_RW_EL1  (0UL << 6)             // access perms: EL1 read/write, no EL0
#define ATTR_SH_NONE    (0UL << 8)             // shareability: none (for device mem)
#define ATTR_SH_INNER   (3UL << 8)             // shareability: inner (for normal mem)
#define ATTR_AF         (1UL << 10)            // Access Flag -- MUST be set, or the
                                               //   first access faults
#define ATTR_PXN        (1UL << 53)            // Privileged eXecute Never
#define ATTR_UXN        (1UL << 54)            // Unprivileged eXecute Never

// ---- MAIR_EL1 memory-type slots ----
// MAIR_EL1 holds up to 8 one-byte "memory type" descriptors. Block entries pick
// one by index (via ATTR_IDX). We define two types:
#define MAIR_IDX_DEVICE 0      // slot 0: device memory (for MMIO)
#define MAIR_IDX_NORMAL 1      // slot 1: normal cacheable RAM
#define MAIR_DEVICE_nGnRnE 0x00   // strongly-ordered device: no gathering/reorder/early-ack
#define MAIR_NORMAL_WB     0xFF   // normal, write-back cacheable, read/write-allocate

#define BLOCK_2M  0x200000UL      // 2 MiB, the size each L2 block entry maps

// The demo's non-identity mapping: virtual 4 GiB will point at physical RAM
// well above the kernel.
#define DEMO_VA   0x100000000UL   // virtual address (4 GiB)
#define DEMO_PA   0x40200000UL    // physical address (free RAM)

// The five page tables. Each is 512 entries of 8 bytes = exactly one 4 KiB page,
// and must be 4 KiB-aligned (the hardware requires table alignment). They live
// in .bss, so they start zeroed (a zero entry = "not mapped").
static uint64_t l0_table[512] __attribute__((aligned(4096)));
static uint64_t l1_table[512] __attribute__((aligned(4096)));
static uint64_t l2_low[512]   __attribute__((aligned(4096)));  // maps VA 0-1GB
static uint64_t l2_high[512]  __attribute__((aligned(4096)));  // maps VA 1-2GB
static uint64_t l2_demo[512]  __attribute__((aligned(4096)));  // the demo mapping

// Build a leaf entry for NORMAL (cacheable RAM) memory at physical address pa.
static uint64_t normal_block(uint64_t pa)
{
    return pa | ATTR_AF | ATTR_SH_INNER | ATTR_AP_RW_EL1 |
           ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_NS | DESC_BLOCK;
}

// Build a leaf entry for DEVICE (MMIO) memory at physical address pa.
// Device memory is marked execute-never (we never run code from a device).
static uint64_t device_block(uint64_t pa)
{
    return pa | ATTR_AF | ATTR_SH_NONE | ATTR_AP_RW_EL1 |
           ATTR_IDX(MAIR_IDX_DEVICE) | ATTR_NS |
           ATTR_PXN | ATTR_UXN | DESC_BLOCK;
}

// Construct the whole page-table tree in memory (the MMU is still OFF here).
static void build_tables(void)
{
    // Top level: the single used L0 entry points at the L1 table.
    l0_table[0] = (uint64_t)l1_table | DESC_TABLE;

    // L1 entries each cover 1 GiB of virtual space. We use three:
    l1_table[0] = (uint64_t)l2_low  | DESC_TABLE;  // VA 0-1GB   -> device map
    l1_table[1] = (uint64_t)l2_high | DESC_TABLE;  // VA 1-2GB   -> RAM map
    l1_table[4] = (uint64_t)l2_demo | DESC_TABLE;  // VA 4-5GB   -> demo map

    // Fill the two identity L2 tables, 512 blocks of 2 MiB each = 1 GiB apiece.
    for (uint64_t i = 0; i < 512; i++) {
        // 0..1 GiB on the virt board is all device/MMIO space (GIC, UART, ...).
        l2_low[i]  = device_block(i * BLOCK_2M);
        // 1..2 GiB is RAM (our kernel lives here). Identity-mapped & cacheable.
        l2_high[i] = normal_block(0x40000000UL + i * BLOCK_2M);
    }

    // The one non-identity mapping: virtual DEMO_VA -> physical DEMO_PA.
    // L2 index for DEMO_VA (4 GiB) is 0, so we only need entry [0].
    l2_demo[0] = normal_block(DEMO_PA);
}

// Program the control registers and switch the MMU on. Order and barriers here
// are critical: the moment translation turns on, the very next instruction
// fetch is translated, so everything must be correct beforehand.
static void enable_mmu(void)
{
    // MAIR_EL1: pack our two memory-type bytes into their slots.
    uint64_t mair =
        ((uint64_t)MAIR_DEVICE_nGnRnE << (8 * MAIR_IDX_DEVICE)) |
        ((uint64_t)MAIR_NORMAL_WB     << (8 * MAIR_IDX_NORMAL));

    // TCR_EL1: translation control -- the "shape" of our address space.
    uint64_t tcr =
        (16UL << 0)  |   // T0SZ=16  -> TTBR0 covers a 48-bit VA range (2^48)
        (1UL  << 8)  |   // IRGN0=WB -> page-table walks use write-back cache
        (1UL  << 10) |   // ORGN0=WB
        (3UL  << 12) |   // SH0=inner shareable
        (0UL  << 14) |   // TG0=0    -> 4 KiB granule
        (1UL  << 23) |   // EPD1=1   -> disable TTBR1 (we only use the low/TTBR0 half)
        (2UL  << 32);    // IPS=2    -> 40-bit physical address size

    // Point the hardware at our tree and load the control regs.
    __asm__ volatile("msr mair_el1,  %0" :: "r"(mair));
    __asm__ volatile("msr tcr_el1,   %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l0_table)); // TTBR0 = top table
    __asm__ volatile("isb");   // make those register writes take effect

    // Flush any stale cached translations from the TLB (translation cache), so
    // the CPU can't honor leftover entries from before our tables existed.
    __asm__ volatile("tlbi vmalle1");   // invalidate all EL1 TLB entries
    __asm__ volatile("dsb ish");        // wait for the invalidate to complete
    __asm__ volatile("isb");            // resync the pipeline

    // Finally flip the enable bits in SCTLR_EL1 (System Control Register).
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);    // M  = enable the MMU (translation ON)
    sctlr |= (1UL << 2);    // C  = enable the data cache
    sctlr |= (1UL << 12);   // I  = enable the instruction cache
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");   // the next instruction is now fetched via the MMU
}

void mmu_init(void)
{
    build_tables();
    __asm__ volatile("dsb ish");   // ensure all table writes are visible to the
                                   //   hardware table walker before we enable it
    enable_mmu();
}
