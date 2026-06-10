// virtio_gpu.c -- the virtio-gpu 2D driver (Phase 25.2).
// ======================================================
//
// virtio-gpu in 2D mode is delightfully dumb: the guest owns a framebuffer in
// its OWN RAM and the device just scans it out. Bring-up is three commands on
// the control queue, then steady state is two per damage rectangle:
//
//   RESOURCE_CREATE_2D    "there exists a wxh BGRX image, call it resource 1"
//   ATTACH_BACKING        "its bytes live at this guest-physical range"
//   SET_SCANOUT           "show resource 1 on display 0"
//   ----------------------------------------------------------------------
//   TRANSFER_TO_HOST_2D   "I changed this rect -- recopy it from my RAM"
//   RESOURCE_FLUSH        "and repaint it on the display"
//
// Every command is a little struct starting with a ctrl_hdr, paired with a
// device-writable response buffer. We submit with virtq_submit (polled), the
// same way virtio-blk does its sector I/O: these commands are rare (bring-up)
// or already batched (one per damage rect, not per pixel), so an IRQ path
// would buy nothing.

#include "gfx.h"
#include "virtio.h"
#include "pmm.h"
#include "seat.h"

#define VIRTIO_ID_GPU 16

// Command types (virtio spec 5.7.6) and the one response we accept.
#define CMD_GET_DISPLAY_INFO   0x0100
#define CMD_RESOURCE_CREATE_2D 0x0101
#define CMD_SET_SCANOUT        0x0103
#define CMD_RESOURCE_FLUSH     0x0104
#define CMD_TRANSFER_TO_HOST   0x0105
#define CMD_ATTACH_BACKING     0x0106
#define CMD_UPDATE_CURSOR      0x0300   // cursor queue: set sprite + position
#define CMD_MOVE_CURSOR        0x0301   // cursor queue: position only
#define RESP_OK_NODATA         0x1100

// Pixel format: bytes B,G,R,X in memory = the little-endian word 0x00RRGGBB,
// which is what every drawing layer above us writes.
#define FORMAT_B8G8R8X8 2
#define FORMAT_B8G8R8A8 1   // with alpha -- the cursor sprite needs holes

struct ctrl_hdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx, pad[3];
};
struct gpu_rect { uint32_t x, y, w, h; };

struct cmd_create_2d {
    struct ctrl_hdr hdr;
    uint32_t resource_id, format, width, height;
};
struct cmd_attach_backing {
    struct ctrl_hdr hdr;
    uint32_t resource_id, nr_entries;
    // followed by nr_entries of:
    uint64_t addr; uint32_t length, padding;    // (we always use exactly one)
};
struct cmd_set_scanout {
    struct ctrl_hdr hdr;
    struct gpu_rect r;
    uint32_t scanout_id, resource_id;
};
struct cmd_transfer {
    struct ctrl_hdr hdr;
    struct gpu_rect r;
    uint64_t offset;
    uint32_t resource_id, padding;
};
struct cmd_flush {
    struct ctrl_hdr hdr;
    struct gpu_rect r;
    uint32_t resource_id, padding;
};

// Cursor-queue command (spec 5.7.6.9): UPDATE binds sprite + position,
// MOVE repositions. The cursor is a hardware overlay PLANE -- it never
// touches the framebuffer, so moving it costs no redisplay at all.
struct cmd_cursor {
    struct ctrl_hdr hdr;
    uint32_t scanout_id, x, y, pos_pad;
    uint32_t resource_id, hot_x, hot_y, padding;
};

static uint64_t     gpu_base;
static struct virtq ctlq;                 // queue 0: the control queue
static struct virtq curq;                 // queue 1: the cursor queue
static int          cursor_ready;
static int          gpu_ok;
static int          shown_id;             // the resource on the scanout (0 = none)

// Submit one command + its response buffer and wait (polled). 0 = device said
// OK. Static response buffer: commands are serialized by our single caller.
static int gpu_cmd(void *cmd, int cmd_len)
{
    static struct ctrl_hdr resp;
    resp.type = 0;
    struct vbuf bufs[2] = {
        { (uint64_t)(uintptr_t)cmd,   (uint32_t)cmd_len, 0 },
        { (uint64_t)(uintptr_t)&resp, sizeof(resp),      1 },
    };
    virtq_submit(gpu_base, &ctlq, bufs, 2);
    return resp.type == RESP_OK_NODATA ? 0 : -1;
}

void gfx_init(void)
{
    // Full reset, including every cached pmm-era value: kmain runs the
    // self-tests at every boot and then re-inits the allocators, so a static
    // surviving gfx_init() would dangle into reused memory (found live in
    // 25.2: the first gfxtest painted red pixels over page tables). The seat
    // table resets here too -- same hazard, same cure.
    gpu_ok = 0;
    shown_id = 0;
    seat_reset();
    gpu_base = virtio_find(VIRTIO_ID_GPU);
    if (!gpu_base) { return; }
    if (virtio_init(gpu_base) != 0) { return; }
    if (virtio_queue_init(gpu_base, &ctlq, 0) != 0) { return; }   // controlq
    if (virtio_queue_init(gpu_base, &curq, 1) != 0) { return; }   // cursorq
    virtio_driver_ok(gpu_base);
    gpu_ok = 1;
    cursor_ready = 0;
}

int gfx_present(void)  { return gpu_ok; }
uint32_t gfx_width(void)  { return GFX_W; }
uint32_t gfx_height(void) { return GFX_H; }

// Create resource `id` (GFX_W x GFX_H) and attach its backing pages. Each
// SEAT gets its own resource + framebuffer, so switching VMs is just a
// SET_SCANOUT to another resource -- no pixel copying between clients.
int gfx_resource_setup(int id, uint64_t fb_phys)
{
    if (!gpu_ok) { return -1; }

    static struct cmd_create_2d c;
    c.hdr.type = CMD_RESOURCE_CREATE_2D; c.hdr.flags = 0; c.hdr.fence_id = 0;
    c.hdr.ctx_id = 0; c.hdr.ring_idx = 0;
    c.resource_id = (uint32_t)id; c.format = FORMAT_B8G8R8X8;
    c.width = GFX_W; c.height = GFX_H;
    if (gpu_cmd(&c, sizeof(c)) != 0) { return -1; }

    static struct cmd_attach_backing a;
    a.hdr.type = CMD_ATTACH_BACKING; a.hdr.flags = 0; a.hdr.fence_id = 0;
    a.hdr.ctx_id = 0; a.hdr.ring_idx = 0;
    a.resource_id = (uint32_t)id; a.nr_entries = 1;
    a.addr = fb_phys; a.length = (uint32_t)GFX_W * GFX_H * 4; a.padding = 0;
    return gpu_cmd(&a, sizeof(a));
}

// Point the scanout at resource `id` and repaint it whole -- what a seat
// switch (or the first client) does.
int gfx_show(int id)
{
    if (!gpu_ok) { return -1; }

    static struct cmd_set_scanout s;
    s.hdr.type = CMD_SET_SCANOUT; s.hdr.flags = 0; s.hdr.fence_id = 0;
    s.hdr.ctx_id = 0; s.hdr.ring_idx = 0;
    s.r.x = 0; s.r.y = 0; s.r.w = GFX_W; s.r.h = GFX_H;
    s.scanout_id = 0; s.resource_id = (uint32_t)id;
    if (gpu_cmd(&s, sizeof(s)) != 0) { return -1; }
    shown_id = id;
    return gfx_flush_rect(0, 0, GFX_W, GFX_H);
}

int gfx_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!gpu_ok || shown_id == 0) { return -1; }

    static struct cmd_transfer t;
    t.hdr.type = CMD_TRANSFER_TO_HOST; t.hdr.flags = 0; t.hdr.fence_id = 0;
    t.hdr.ctx_id = 0; t.hdr.ring_idx = 0;
    t.r.x = x; t.r.y = y; t.r.w = w; t.r.h = h;
    // The transfer source offset must point at the rect's first pixel within
    // the resource, or the device would copy from the frame's origin.
    t.offset = ((uint64_t)y * GFX_W + x) * 4;
    t.resource_id = (uint32_t)shown_id; t.padding = 0;
    if (gpu_cmd(&t, sizeof(t)) != 0) { return -1; }

    static struct cmd_flush f;
    f.hdr.type = CMD_RESOURCE_FLUSH; f.hdr.flags = 0; f.hdr.fence_id = 0;
    f.hdr.ctx_id = 0; f.hdr.ring_idx = 0;
    f.r.x = x; f.r.y = y; f.r.w = w; f.r.h = h;
    f.resource_id = (uint32_t)shown_id; f.padding = 0;
    return gpu_cmd(&f, sizeof(f));
}

// ---- the mouse cursor: a hardware overlay plane ---------------------------

#define CURSOR_RES 100                    // resource id reserved for the sprite
static uint32_t cursor_px[64 * 64];       // B8G8R8A8 sprite (DMA)

// The classic arrow pointer -- the canonical notched-tail polygon, white
// fill with a black outline, transparent elsewhere. Drawn from an ASCII
// bitmap ('X' outline, '.' fill) at 2x scale so it carries presence at
// 1280x720. An ASCII sprite is an asset and source code at the same time.
static const char *cursor_art[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "       XX   ",
};

static void cursor_sprite(void)
{
    for (int i = 0; i < 64 * 64; i++) { cursor_px[i] = 0; }   // transparent
    int rows = (int)(sizeof(cursor_art) / sizeof(cursor_art[0]));
    for (int y = 0; y < rows; y++) {
        for (int x = 0; cursor_art[y][x]; x++) {
            uint32_t c = 0;
            if (cursor_art[y][x] == 'X') { c = 0xFF000000; }       // outline
            if (cursor_art[y][x] == '.') { c = 0xFFFFFFFF; }       // fill
            if (!c) { continue; }
            for (int dy = 0; dy < 2; dy++) {                       // 2x scale
                for (int dx = 0; dx < 2; dx++) {
                    cursor_px[(y * 2 + dy) * 64 + (x * 2 + dx)] = c;
                }
            }
        }
    }
}

static int cursor_cmd(uint32_t type, int x, int y)
{
    static struct cmd_cursor c;
    static struct ctrl_hdr resp;
    c.hdr.type = type; c.hdr.flags = 0; c.hdr.fence_id = 0;
    c.hdr.ctx_id = 0; c.hdr.ring_idx = 0;
    c.scanout_id = 0; c.x = (uint32_t)x; c.y = (uint32_t)y; c.pos_pad = 0;
    c.resource_id = CURSOR_RES; c.hot_x = 0; c.hot_y = 0; c.padding = 0;
    resp.type = 0;
    struct vbuf bufs[2] = {
        { (uint64_t)(uintptr_t)&c,    sizeof(c),    0 },
        { (uint64_t)(uintptr_t)&resp, sizeof(resp), 1 },
    };
    // Unlike the control queue, QEMU completes cursor-queue commands WITHOUT
    // writing a response struct -- completion itself is the acknowledgement.
    virtq_submit(gpu_base, &curq, bufs, 2);
    return 0;
}

// Bind the sprite (lazily, once a scanout exists) and show it at (x, y).
int gfx_cursor_move(int x, int y)
{
    if (!gpu_ok || shown_id == 0) { return -1; }
    if (!cursor_ready) {
        cursor_sprite();
        static struct cmd_create_2d c;
        c.hdr.type = CMD_RESOURCE_CREATE_2D; c.hdr.flags = 0; c.hdr.fence_id = 0;
        c.hdr.ctx_id = 0; c.hdr.ring_idx = 0;
        c.resource_id = CURSOR_RES; c.format = FORMAT_B8G8R8A8;
        c.width = 64; c.height = 64;
        if (gpu_cmd(&c, sizeof(c)) != 0) { return -1; }
        static struct cmd_attach_backing a;
        a.hdr.type = CMD_ATTACH_BACKING; a.hdr.flags = 0; a.hdr.fence_id = 0;
        a.hdr.ctx_id = 0; a.hdr.ring_idx = 0;
        a.resource_id = CURSOR_RES; a.nr_entries = 1;
        a.addr = (uint64_t)(uintptr_t)cursor_px; a.length = sizeof(cursor_px);
        a.padding = 0;
        if (gpu_cmd(&a, sizeof(a)) != 0) { return -1; }
        static struct cmd_transfer t;
        t.hdr.type = CMD_TRANSFER_TO_HOST; t.hdr.flags = 0; t.hdr.fence_id = 0;
        t.hdr.ctx_id = 0; t.hdr.ring_idx = 0;
        t.r.x = 0; t.r.y = 0; t.r.w = 64; t.r.h = 64;
        t.offset = 0; t.resource_id = CURSOR_RES; t.padding = 0;
        if (gpu_cmd(&t, sizeof(t)) != 0) { return -1; }
        if (cursor_cmd(CMD_UPDATE_CURSOR, x, y) != 0) { return -1; }
        cursor_ready = 1;
        return 0;
    }
    return cursor_cmd(CMD_MOVE_CURSOR, x, y);
}

// A fresh client framebuffer: contiguous (one ATTACH_BACKING entry), zeroed.
// Every seat gets its own -- 3.7 MB each, four seats fit easily in 256 MB.
uint64_t gfx_fb_new(void)
{
    if (!gpu_ok) { return 0; }
    uint64_t npages = ((uint64_t)GFX_W * GFX_H * 4 + 4095) / 4096;
    void *p = pmm_alloc_pages(npages);
    if (!p) { return 0; }
    uint64_t *words = p;
    for (uint64_t i = 0; i < (uint64_t)GFX_W * GFX_H * 4 / 8; i++) { words[i] = 0; }
    return (uint64_t)(uintptr_t)p;
}
