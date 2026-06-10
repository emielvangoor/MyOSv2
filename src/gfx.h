// gfx.h -- virtio-gpu 2D: a dumb scanout for a guest-built framebuffer.
// ======================================================================
// The display side of the graphical Lisp machine (Phase 25.2). The device
// never draws anything: the GUEST renders into ordinary RAM, then tells the
// device "bytes changed in this rect -- recopy and show them". All policy
// (layout, glyphs, windows) lives above, in user space.
#pragma once
#include <stdint.h>

#define GFX_W 1280
#define GFX_H 720

void gfx_init(void);                     // find + reset + queues (called by kmain)
int  gfx_present(void);
uint32_t gfx_width(void);
uint32_t gfx_height(void);

// Point scanout 0 at a guest framebuffer: physical, CONTIGUOUS, w*h*4 bytes,
// BGRX little-endian (a pixel is the u32 word 0x00RRGGBB). Returns 0 on success.
int  gfx_setup(uint64_t fb_phys, uint32_t w, uint32_t h);

// Push one damage rect to the display: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
int  gfx_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Allocate (once) the GFX_W x GFX_H kernel framebuffer as contiguous pages and
// point the scanout at it; returns its physical address (0 on failure). The
// gfx_acquire syscall maps these same pages into the calling process.
uint64_t gfx_fb_alloc(void);
