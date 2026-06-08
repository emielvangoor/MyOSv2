// fwcfg.h -- QEMU fw_cfg device: pass config blobs from QEMU to the guest.
// =======================================================================
//
// fw_cfg is a tiny MMIO device (base 0x09020000 on the virt board) that exposes
// named "items" -- little configuration blobs QEMU wants the guest to see. We
// use it for exactly one thing: registering a `ramfb` framebuffer (telling QEMU
// "scan out the pixels at THIS physical address, THIS geometry").
//
// THE GOLDEN RULE: every multi-byte fw_cfg value is BIG-ENDIAN. ARM runs
// little-endian, so every field we hand the device -- or read back from it --
// must be byte-swapped. The helpers below do that; forget one and you get
// garbage geometry (or a silent no-display).
#pragma once
#include <stdint.h>

// Byte-swap helpers (host little-endian <-> fw_cfg big-endian).
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

// Look up an item's 16-bit selector key by name (e.g. "etc/ramfb"); returns -1
// if the item is absent (e.g. QEMU started without `-device ramfb`). On success,
// *size_out (if non-NULL) receives the item's byte size.
int  fwcfg_find_file(const char *name, uint32_t *size_out);

// DMA-transfer to/from a selected item. `control` carries the selector key in
// bits [31:16] together with the SELECT bit and a READ or WRITE bit. Returns 0
// on success, -1 if the device reports an error.
int  fwcfg_dma(uint32_t control, uint32_t length, void *buf);

// fw_cfg DMA control bits (see the virtio/fw_cfg spec).
#define FWCFG_DMA_ERROR  0x01u   // set by the device on failure
#define FWCFG_DMA_READ   0x02u   // transfer fw_cfg -> memory
#define FWCFG_DMA_SKIP   0x04u   // advance the read offset without copying
#define FWCFG_DMA_SELECT 0x08u   // (re)select the item named in bits [31:16]
#define FWCFG_DMA_WRITE  0x10u   // transfer memory -> fw_cfg
#define FWCFG_FILE_DIR   0x0019u // selector key of the file directory
