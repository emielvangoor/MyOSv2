// gfxtest.c -- /bin/gfxtest: prove a userland process can paint pixels.
// =========================================================================
// Draws an unmistakable test pattern -- red / green / blue vertical thirds +
// a white 8x8 square at (8,8). Two modes:
//
//   * SURFACE mode (run from the frame via run-in-buffer): argv is
//     {prog, shm-handle, w, h} -- we map the shm canvas and draw into it, the
//     same contract the teapot uses, so the pattern shows IN a frame buffer
//     (the frame's redisplay blits surface canvases). Use `(gfxtest)`.
//   * STANDALONE mode (no surface args): acquire the raw framebuffer and draw
//     there. This is the Phase 25.2 probe tools/gfx_check.py screendumps.
//
// (Drawing straight to the raw fb does NOT show inside the frame: the redisplay
// engine owns the framebuffer and repaints over it every cycle. Hence the
// surface path -- the supported way for a program to put graphics in a buffer.)
#include "ulib.h"

static long parse(const char *s)
{ long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } return v; }

// Paint the RGB-thirds + white-square pattern into a w*h buffer of `stride`
// 32-bit pixels per row.
static void draw(unsigned int *fb, unsigned int w, unsigned int h, unsigned int stride)
{
    for (unsigned int y = 0; y < h; y++) {
        for (unsigned int x = 0; x < w; x++) {
            fb[y * stride + x] = x < w / 3     ? 0x00FF0000   // red third
                               : x < 2 * w / 3 ? 0x0000FF00   // green third
                                               : 0x000000FF;  // blue third
        }
    }
    for (unsigned int y = 8; y < 16 && y < h; y++) {
        for (unsigned int x = 8; x < 16 && x < w; x++) { fb[y * stride + x] = 0x00FFFFFF; }
    }
}

int umain(int argc, char **argv)
{
    if (argc >= 4) {
        // Surface mode: run-in-buffer handed us {handle, w, h}.
        int handle = (int)parse(argv[1]);
        unsigned int w = (unsigned int)parse(argv[2]);
        unsigned int h = (unsigned int)parse(argv[3]);
        unsigned int *canvas = shm_map(handle);
        if (!canvas) { sys_write(1, "gfxtest: shm_map failed\n", 24); return 1; }
        draw(canvas, w, h, w);                 // surface canvas: stride == width
        sys_write(1, "gfxtest: pattern drawn (surface)\n", 33);
        for (;;) { sys_sleep(1000); }          // hold so the frame keeps showing it
    }

    // Standalone mode: the raw framebuffer (tools/gfx_check.py screendumps it).
    struct gfx_info gi;
    if (gfx_acquire(&gi) != 0) {
        sys_write(1, "gfxtest: no gpu\n", 16);
        return 1;
    }
    draw(gi.fb, gi.w, gi.h, gi.pitch / 4);
    gfx_flush(0, 0, (int)gi.w, (int)gi.h);
    sys_write(1, "gfxtest: pattern drawn\n", 23);
    for (;;) { sys_sleep(1000); }              // hold the image for the check
}
