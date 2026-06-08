// fwcfg.c -- QEMU fw_cfg MMIO + DMA driver (big-endian wire format).
// ==================================================================
//
// We talk to fw_cfg only through its DMA interface: build a small control block
// in RAM describing the transfer (which item, read or write, how many bytes,
// where), then write that block's physical address to the DMA register. QEMU
// performs the transfer immediately and writes a status back into the block.
//
// Remember: every fw_cfg field on the wire is BIG-ENDIAN, so each value is
// byte-swapped going out and coming back.

#include <stdint.h>
#include "fwcfg.h"

#define FWCFG_BASE 0x09020000UL
// The DMA address register (64-bit). Writing a control-block address here kicks
// off the transfer. It's defined big-endian, so we store a byte-swapped address.
#define FWCFG_DMA  (*(volatile uint64_t *)(FWCFG_BASE + 0x10))

// The control block the device reads and updates; all fields big-endian.
struct fwcfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed));

// A single shared block in .bss (== identity-mapped RAM, so its kernel address
// is also its physical address -- exactly what the device needs).
static volatile struct fwcfg_dma_access dma;

int fwcfg_dma(uint32_t control, uint32_t length, void *buf)
{
    dma.control = bswap32(control);
    dma.length  = bswap32(length);
    dma.address = bswap64((uint64_t)(uintptr_t)buf);
    __asm__ volatile("dsb sy" ::: "memory");           // publish the block first
    FWCFG_DMA = bswap64((uint64_t)(uintptr_t)&dma);    // kick the transfer
    __asm__ volatile("dsb sy" ::: "memory");
    // QEMU completes synchronously; the device clears the busy bits when done.
    while (bswap32(dma.control) & ~FWCFG_DMA_ERROR) {
        // spin until only the (possible) error bit remains
    }
    return (bswap32(dma.control) & FWCFG_DMA_ERROR) ? -1 : 0;
}

// Compare a NUL-terminated name against a (possibly NUL-padded) fixed field.
static int name_eq(const char *field, const char *want)
{
    while (*want) {
        if (*field != *want) { return 0; }
        field++; want++;
    }
    return *field == '\0';
}

int fwcfg_find_file(const char *name, uint32_t *size_out)
{
    // SELECT the file directory and read its 4-byte entry count.
    uint32_t count_be;
    if (fwcfg_dma((FWCFG_FILE_DIR << 16) | FWCFG_DMA_SELECT | FWCFG_DMA_READ,
                  sizeof(count_be), &count_be) != 0) {
        return -1;
    }
    uint32_t count = bswap32(count_be);

    // Each subsequent (no-SELECT) read advances through the directory entries.
    struct {
        uint32_t size;
        uint16_t select;
        uint16_t reserved;
        char     name[56];
    } __attribute__((packed)) entry;

    for (uint32_t i = 0; i < count; i++) {
        if (fwcfg_dma(FWCFG_DMA_READ, sizeof(entry), &entry) != 0) {
            return -1;
        }
        if (name_eq(entry.name, name)) {
            if (size_out) { *size_out = bswap32(entry.size); }
            return (int)bswap16(entry.select);
        }
    }
    return -1;   // not present (e.g. no `-device ramfb`)
}
