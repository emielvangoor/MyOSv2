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

#define VIRTIO_ID_GPU 16

// Command types (virtio spec 5.7.6) and the one response we accept.
#define CMD_GET_DISPLAY_INFO   0x0100
#define CMD_RESOURCE_CREATE_2D 0x0101
#define CMD_SET_SCANOUT        0x0103
#define CMD_RESOURCE_FLUSH     0x0104
#define CMD_TRANSFER_TO_HOST   0x0105
#define CMD_ATTACH_BACKING     0x0106
#define RESP_OK_NODATA         0x1100

// Pixel format: bytes B,G,R,X in memory = the little-endian word 0x00RRGGBB,
// which is what every drawing layer above us writes.
#define FORMAT_B8G8R8X8 2

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

static uint64_t     gpu_base;
static struct virtq ctlq;                 // queue 0: the control queue
static int          gpu_ok;
static uint32_t     fb_w, fb_h;           // what the scanout currently shows
static uint64_t     fb_pa;                // the kernel framebuffer (gfx_fb_alloc)

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
    // Full reset, including the cached framebuffer: kmain runs the self-tests
    // at every boot and then re-inits the allocators, so any pmm-era pointer
    // cached across a gfx_init() would dangle into reused memory (found live:
    // the first gfxtest run painted red pixels over the real boot's page
    // tables). gfx_init() is called after the reset and must forget the past.
    gpu_ok = 0;
    fb_pa = 0;
    fb_w = 0; fb_h = 0;
    gpu_base = virtio_find(VIRTIO_ID_GPU);
    if (!gpu_base) { return; }
    if (virtio_init(gpu_base) != 0) { return; }
    if (virtio_queue_init(gpu_base, &ctlq, 0) != 0) { return; }   // controlq
    virtio_driver_ok(gpu_base);
    gpu_ok = 1;
}

int gfx_present(void)  { return gpu_ok; }
uint32_t gfx_width(void)  { return GFX_W; }
uint32_t gfx_height(void) { return GFX_H; }

int gfx_setup(uint64_t fb_phys, uint32_t w, uint32_t h)
{
    if (!gpu_ok) { return -1; }

    static struct cmd_create_2d c;
    c.hdr.type = CMD_RESOURCE_CREATE_2D; c.hdr.flags = 0; c.hdr.fence_id = 0;
    c.hdr.ctx_id = 0; c.hdr.ring_idx = 0;
    c.resource_id = 1; c.format = FORMAT_B8G8R8X8; c.width = w; c.height = h;
    if (gpu_cmd(&c, sizeof(c)) != 0) { return -1; }

    static struct cmd_attach_backing a;
    a.hdr.type = CMD_ATTACH_BACKING; a.hdr.flags = 0; a.hdr.fence_id = 0;
    a.hdr.ctx_id = 0; a.hdr.ring_idx = 0;
    a.resource_id = 1; a.nr_entries = 1;
    a.addr = fb_phys; a.length = w * h * 4; a.padding = 0;
    if (gpu_cmd(&a, sizeof(a)) != 0) { return -1; }

    static struct cmd_set_scanout s;
    s.hdr.type = CMD_SET_SCANOUT; s.hdr.flags = 0; s.hdr.fence_id = 0;
    s.hdr.ctx_id = 0; s.hdr.ring_idx = 0;
    s.r.x = 0; s.r.y = 0; s.r.w = w; s.r.h = h;
    s.scanout_id = 0; s.resource_id = 1;
    if (gpu_cmd(&s, sizeof(s)) != 0) { return -1; }

    fb_w = w; fb_h = h;
    return 0;
}

int gfx_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!gpu_ok || fb_w == 0) { return -1; }

    static struct cmd_transfer t;
    t.hdr.type = CMD_TRANSFER_TO_HOST; t.hdr.flags = 0; t.hdr.fence_id = 0;
    t.hdr.ctx_id = 0; t.hdr.ring_idx = 0;
    t.r.x = x; t.r.y = y; t.r.w = w; t.r.h = h;
    // The transfer source offset must point at the rect's first pixel within
    // the resource, or the device would copy from the frame's origin.
    t.offset = ((uint64_t)y * fb_w + x) * 4;
    t.resource_id = 1; t.padding = 0;
    if (gpu_cmd(&t, sizeof(t)) != 0) { return -1; }

    static struct cmd_flush f;
    f.hdr.type = CMD_RESOURCE_FLUSH; f.hdr.flags = 0; f.hdr.fence_id = 0;
    f.hdr.ctx_id = 0; f.hdr.ring_idx = 0;
    f.r.x = x; f.r.y = y; f.r.w = w; f.r.h = h;
    f.resource_id = 1; f.padding = 0;
    return gpu_cmd(&f, sizeof(f));
}

// The kernel-owned framebuffer behind the gfx_acquire syscall. Allocated once
// per gfx_init era (contiguous, so ATTACH_BACKING needs a single entry) and
// then shared: a second caller gets the same buffer -- there is one screen.
uint64_t gfx_fb_alloc(void)
{
    if (!gpu_ok) { return 0; }
    if (fb_pa) { return fb_pa; }
    uint64_t npages = ((uint64_t)GFX_W * GFX_H * 4 + 4095) / 4096;
    void *p = pmm_alloc_pages(npages);
    if (!p) { return 0; }
    uint64_t *words = p;
    for (uint64_t i = 0; i < (uint64_t)GFX_W * GFX_H * 4 / 8; i++) { words[i] = 0; }
    if (gfx_setup((uint64_t)(uintptr_t)p, GFX_W, GFX_H) != 0) { return 0; }
    fb_pa = (uint64_t)(uintptr_t)p;
    return fb_pa;
}
