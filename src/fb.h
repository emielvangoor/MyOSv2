// fb.h -- a 1280x720 XRGB8888 framebuffer, scanned out by QEMU's ramfb device.
// ===========================================================================
//
// A "framebuffer" is just a big array of pixels in RAM: one 32-bit word per
// pixel, laid out row by row. The display device (ramfb) reads that array many
// times a second and paints it to a window. So "drawing" is nothing more than
// writing the right 32-bit values into the right slots of this array.
#pragma once
#include <stdint.h>

#define FB_WIDTH        1280
#define FB_HEIGHT       720
#define FB_BPP          4                            // bytes per pixel (XRGB8888)
#define FB_PITCH_PX     FB_WIDTH                      // pixels to advance one row
#define FB_STRIDE_BYTES (FB_WIDTH * FB_BPP)           // bytes per row = 5120
#define FB_PAGES        ((FB_WIDTH * FB_HEIGHT * FB_BPP) / 4096)   // 900 (page-exact)

#define DRM_FORMAT_XRGB8888 0x34325258u               // fourcc 'XR24'

// What the kernel draws into.
struct fb_info {
    volatile uint32_t *pixels;   // base of the framebuffer (identity-mapped RAM)
    uint32_t width, height;
    uint32_t pitch_px;           // pixels to advance one row (== width here)
};

// The ramfb configuration blob handed to QEMU via fw_cfg. ALL fields are
// big-endian (packed so there's no padding -- QEMU reads it byte-for-byte).
struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} __attribute__((packed));

// Fill `cfg` (byte-swapped to big-endian) describing a framebuffer at physical
// address `addr`, geometry w x h, XRGB8888. Pure -- no hardware touched.
void ramfb_build_cfg(struct ramfb_cfg *cfg, uint64_t addr, uint32_t w, uint32_t h);

// Allocate the framebuffer and register it with ramfb over fw_cfg. Returns 1 on
// success (and fills *out), or 0 if ramfb is absent (no display). Never hangs.
int  fb_init(struct fb_info *out);
