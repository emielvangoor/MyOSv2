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

// Each SEAT (display client) gets its own resource + framebuffer; switching
// VMs is a SET_SCANOUT, never a pixel copy.
int  gfx_resource_setup(int id, uint64_t fb_phys);   // create + attach backing
int  gfx_show(int id);                               // scanout to id + full repaint
// Push one damage rect of the CURRENTLY SHOWN resource to the display.
int  gfx_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
// A fresh, zeroed, contiguous GFX_W x GFX_H framebuffer (physical addr; 0 = OOM).
uint64_t gfx_fb_new(void);
