// draw.h -- pixel drawing primitives over a framebuffer (pure pixel math).
// ========================================================================
//
// These operate on any `struct fb_info` -- the real framebuffer in a normal run,
// or a small fake buffer in the unit tests. Drawing = writing 0x00RRGGBB words
// into the pixel array; every primitive bounds-checks so nothing scribbles past
// the buffer.
#pragma once
#include <stdint.h>
#include "fb.h"

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);   // pack 8-bit channels -> 0x00RRGGBB
void draw_put(struct fb_info *f, int x, int y, uint32_t color);          // one pixel (clipped)
void draw_fill_rect(struct fb_info *f, int x, int y, int w, int h, uint32_t color); // a rectangle (clipped)
void draw_clear(struct fb_info *f, uint32_t color);                      // whole screen
void draw_char(struct fb_info *f, int x, int y, char ch, uint32_t fg);   // one glyph (8x8 scaled 2x)
void draw_text(struct fb_info *f, int x, int y, const char *s, uint32_t fg); // a string
