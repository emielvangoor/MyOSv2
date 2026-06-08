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
#include "pmm.h"

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

int fb_init(struct fb_info *out)
{
    // Is there a ramfb device? (Did QEMU start with `-device ramfb`?) If not,
    // there's nothing to scan out our pixels -- report "no display" and let the
    // caller carry on headless.
    int key = fwcfg_find_file("etc/ramfb", 0);
    if (key < 0) {
        return 0;
    }

    // Allocate the pixel buffer as a contiguous run in identity-mapped RAM, so
    // the physical address we give QEMU is also a valid kernel pointer to draw
    // through.
    void *base = pmm_alloc_pages(FB_PAGES);
    if (!base) {
        return 0;
    }

    // Hand QEMU the address + geometry; from now it scans out our buffer.
    struct ramfb_cfg cfg;
    ramfb_build_cfg(&cfg, (uint64_t)(uintptr_t)base, FB_WIDTH, FB_HEIGHT);
    if (fwcfg_dma(((uint32_t)key << 16) | FWCFG_DMA_SELECT | FWCFG_DMA_WRITE,
                  sizeof(cfg), &cfg) != 0) {
        return 0;
    }

    out->pixels   = (volatile uint32_t *)base;
    out->width    = FB_WIDTH;
    out->height   = FB_HEIGHT;
    out->pitch_px = FB_PITCH_PX;
    return 1;
}
