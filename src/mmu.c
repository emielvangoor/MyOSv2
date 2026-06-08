#include <stdint.h>
#include "mmu.h"

// ---- Descriptor low bits ----
#define DESC_TABLE      (3UL << 0)   // points to a next-level table (0b11)
#define DESC_BLOCK      (1UL << 0)   // a block mapping (0b01)

// ---- Block/page attribute bits ----
#define ATTR_IDX(n)     ((uint64_t)(n) << 2)   // index into MAIR_EL1
#define ATTR_NS         (0UL << 5)
#define ATTR_AP_RW_EL1  (0UL << 6)             // EL1 read/write, no EL0
#define ATTR_SH_NONE    (0UL << 8)
#define ATTR_SH_INNER   (3UL << 8)             // inner shareable
#define ATTR_AF         (1UL << 10)            // access flag (required)
#define ATTR_PXN        (1UL << 53)            // privileged execute-never
#define ATTR_UXN        (1UL << 54)            // unprivileged execute-never

// ---- MAIR attribute slots ----
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1
#define MAIR_DEVICE_nGnRnE 0x00
#define MAIR_NORMAL_WB     0xFF

#define BLOCK_2M  0x200000UL
#define DEMO_VA   0x100000000UL
#define DEMO_PA   0x40200000UL

// Five 4 KiB-aligned tables (zeroed in .bss).
static uint64_t l0_table[512] __attribute__((aligned(4096)));
static uint64_t l1_table[512] __attribute__((aligned(4096)));
static uint64_t l2_low[512]   __attribute__((aligned(4096)));  // 0-1GB devices
static uint64_t l2_high[512]  __attribute__((aligned(4096)));  // 1-2GB RAM
static uint64_t l2_demo[512]  __attribute__((aligned(4096)));  // demo mapping

static uint64_t normal_block(uint64_t pa)
{
    return pa | ATTR_AF | ATTR_SH_INNER | ATTR_AP_RW_EL1 |
           ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_NS | DESC_BLOCK;
}

static uint64_t device_block(uint64_t pa)
{
    return pa | ATTR_AF | ATTR_SH_NONE | ATTR_AP_RW_EL1 |
           ATTR_IDX(MAIR_IDX_DEVICE) | ATTR_NS |
           ATTR_PXN | ATTR_UXN | DESC_BLOCK;
}

static void build_tables(void)
{
    l0_table[0] = (uint64_t)l1_table | DESC_TABLE;

    l1_table[0] = (uint64_t)l2_low  | DESC_TABLE;  // VA 0-1GB
    l1_table[1] = (uint64_t)l2_high | DESC_TABLE;  // VA 1-2GB
    l1_table[4] = (uint64_t)l2_demo | DESC_TABLE;  // VA 4-5GB (demo)

    for (uint64_t i = 0; i < 512; i++) {
        l2_low[i]  = device_block(i * BLOCK_2M);                 // identity, Device
        l2_high[i] = normal_block(0x40000000UL + i * BLOCK_2M);  // identity, Normal
    }

    l2_demo[0] = normal_block(DEMO_PA);   // VA 0x100000000 -> PA 0x40200000
}

static void enable_mmu(void)
{
    uint64_t mair =
        ((uint64_t)MAIR_DEVICE_nGnRnE << (8 * MAIR_IDX_DEVICE)) |
        ((uint64_t)MAIR_NORMAL_WB     << (8 * MAIR_IDX_NORMAL));

    uint64_t tcr =
        (16UL << 0)  |   // T0SZ = 16 -> 48-bit VA
        (1UL  << 8)  |   // IRGN0 = write-back, write-allocate
        (1UL  << 10) |   // ORGN0 = write-back, write-allocate
        (3UL  << 12) |   // SH0   = inner shareable
        (0UL  << 14) |   // TG0   = 4 KiB granule
        (1UL  << 23) |   // EPD1  = disable TTBR1 walks
        (2UL  << 32);    // IPS   = 40-bit physical addresses

    __asm__ volatile("msr mair_el1,  %0" :: "r"(mair));
    __asm__ volatile("msr tcr_el1,   %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l0_table));
    __asm__ volatile("isb");

    __asm__ volatile("tlbi vmalle1");   // flush stale EL1 TLB entries
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);    // M  - enable MMU
    sctlr |= (1UL << 2);    // C  - data cache
    sctlr |= (1UL << 12);   // I  - instruction cache
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");
}

void mmu_init(void)
{
    build_tables();
    __asm__ volatile("dsb ish");   // ensure table writes are visible to walks
    enable_mmu();
}
