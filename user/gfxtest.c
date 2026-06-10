// gfxtest.c -- /bin/gfxtest: prove a userland process can paint the screen.
// =========================================================================
// The Phase 25.2 demo + integration probe: acquire the framebuffer, draw an
// unmistakable pattern (red / green / blue vertical thirds + a white square
// at (8,8)), flush, and sit still so tools/gfx_check.py can screendump the
// display and assert on the pixels.
#include "ulib.h"

int umain(void)
{
    struct gfx_info gi;
    if (gfx_acquire(&gi) != 0) {
        sys_write(1, "gfxtest: no gpu\n", 16);
        return 1;
    }

    unsigned int *fb = gi.fb;
    unsigned int stride = gi.pitch / 4;
    for (unsigned int y = 0; y < gi.h; y++) {
        for (unsigned int x = 0; x < gi.w; x++) {
            fb[y * stride + x] = x < gi.w / 3     ? 0x00FF0000   // red third
                               : x < 2 * gi.w / 3 ? 0x0000FF00   // green third
                                                  : 0x000000FF;  // blue third
        }
    }
    for (unsigned int y = 8; y < 16; y++) {
        for (unsigned int x = 8; x < 16; x++) { fb[y * stride + x] = 0x00FFFFFF; }
    }

    gfx_flush(0, 0, (int)gi.w, (int)gi.h);
    sys_write(1, "gfxtest: pattern drawn\n", 23);
    for (;;) { sys_sleep(1000); }              // hold the image for the check
}
