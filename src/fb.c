// fb.c -- allocate a framebuffer and register it with QEMU's ramfb device.
// ========================================================================
//
// Two jobs:
//   1. ramfb_build_cfg() -- pack the geometry into the big-endian blob QEMU
//      wants (pure, unit-tested).
//   2. fb_init() -- allocate the pixel memory, find ramfb in fw_cfg, and DMA the
//      blob to it so QEMU starts scanning out our pixels (hardware; verified by
//      the visible window).

#include <stdint.h>
#include "fb.h"
#include "fwcfg.h"

void ramfb_build_cfg(struct ramfb_cfg *cfg, uint64_t addr, uint32_t w, uint32_t h)
{
    // Every field byte-swapped: QEMU reads this blob as big-endian.
    cfg->addr   = bswap64(addr);
    cfg->fourcc = bswap32(DRM_FORMAT_XRGB8888);
    cfg->flags  = 0;
    cfg->width  = bswap32(w);
    cfg->height = bswap32(h);
    cfg->stride = bswap32(w * FB_BPP);
}

// fb_init() is added in Task 5, once the fw_cfg driver (fwcfg.c) exists.
