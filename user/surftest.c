// surftest.c -- /bin/surftest: an external program rendering into a buffer.
// =========================================================================
// The other half of (run-in-buffer ...): argv hands us a shared-memory handle
// and the canvas geometry; we shm_map it and draw. We never touch the display
// -- the Lisp machine's redisplay blits this canvas into whichever window
// shows the buffer. A graphical program as an Emacs buffer: the EXWM deal.
#include "ulib.h"

static long parse(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int umain(int argc, char **argv)
{
    if (argc < 4) { sys_write(1, "surftest: need handle w h\n", 26); return 1; }
    int handle = (int)parse(argv[1]);
    int w = (int)parse(argv[2]), h = (int)parse(argv[3]);
    unsigned int *cv = shm_map(handle);
    if (!cv) { sys_write(1, "surftest: shm_map failed\n", 25); return 1; }

    // A green field with a white frame and a magenta square: unmistakable.
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned int c = 0x0000CC44;                       // green
            if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) { c = 0x00FFFFFF; }
            if (x >= 20 && x < 60 && y >= 20 && y < 60)      { c = 0x00FF00FF; }
            cv[y * w + x] = c;
        }
    }
    sys_write(1, "surftest: drawn\n", 16);
    for (;;) { sys_sleep(1000); }              // stay resident, like a real app
}
