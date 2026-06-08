// draw.c -- pixel drawing primitives over a framebuffer.
// ======================================================
//
// All drawing funnels through draw_put(), which clips to the framebuffer. So
// every higher primitive (rectangles, text) is automatically safe at the edges:
// off-screen pixels are simply dropped.

#include <stdint.h>
#include "draw.h"
#include "font8x8.h"

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    // XRGB8888: top byte unused, then red, green, blue.
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void draw_put(struct fb_info *f, int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= f->width || (uint32_t)y >= f->height) {
        return;                                  // off-screen -> ignore
    }
    f->pixels[(uint32_t)y * f->pitch_px + (uint32_t)x] = color;
}

void draw_fill_rect(struct fb_info *f, int x, int y, int w, int h, uint32_t color)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            draw_put(f, x + i, y + j, color);    // draw_put clips each pixel
        }
    }
}

void draw_clear(struct fb_info *f, uint32_t color)
{
    draw_fill_rect(f, 0, 0, (int)f->width, (int)f->height, color);
}

// Draw one 8x8 glyph scaled 2x (a 16x16 cell, for legibility at this resolution).
// In font8x8_basic the bit order is LSB = leftmost column, so we test (1 << col).
void draw_char(struct fb_info *f, int x, int y, char ch, uint32_t fg)
{
    const uint8_t *g = font_glyph(ch);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                // Each font pixel becomes a 2x2 block.
                draw_put(f, x + col * 2,     y + row * 2,     fg);
                draw_put(f, x + col * 2 + 1, y + row * 2,     fg);
                draw_put(f, x + col * 2,     y + row * 2 + 1, fg);
                draw_put(f, x + col * 2 + 1, y + row * 2 + 1, fg);
            }
        }
    }
}

void draw_text(struct fb_info *f, int x, int y, const char *s, uint32_t fg)
{
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += 18; continue; }   // next line
        draw_char(f, cx, y, *s, fg);
        cx += 18;                                        // 16px glyph + 2px gap
    }
}
