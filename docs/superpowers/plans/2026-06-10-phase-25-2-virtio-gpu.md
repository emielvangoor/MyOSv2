# Phase 25.2 — virtio-gpu + gfx_acquire/gfx_flush Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A virtio-gpu 2D driver and the `gfx_acquire`/`gfx_flush` syscalls: a userland process gets a mapped 1280×720 BGRX framebuffer and pushes damage rectangles to the QEMU display, proven end-to-end by a QMP `screendump` pixel check.

**Architecture:** DeviceID 16 on the existing virtio transport; the controlq carries little command structs (create resource → attach backing → set scanout; then transfer+flush per damage rect), each submitted with `virtq_submit` (polled, like virtio-blk). The kernel allocates the framebuffer as contiguous pages (one ATTACH_BACKING entry) and maps it into the calling process with `as_map_phys` — the Phase-16 shm machinery's pattern. Spec: `docs/superpowers/specs/2026-06-10-graphical-lisp-machine-design.md`.

**Tech Stack:** C (freestanding), virtio-mmio, KTEST, QEMU QMP screendump + PPM parsing in the Python harness.

---

### Task 1: the driver core (`src/virtio_gpu.c` + `src/gfx.h`)

**Files:** Create `src/gfx.h`, `src/virtio_gpu.c`. Modify `Makefile` (QEMU_FLAGS gains `-device virtio-gpu-device`), `src/kmain.c` (init + banner). Test: `src/tests.c`.

- [ ] **Step 1: failing KTESTs** (registry: `gpu: device present`, `gpu: scanout configured`):

```c
static void test_gpu_present(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    KASSERT(gfx_present());
    KASSERT(gfx_width() == 1280 && gfx_height() == 720);
}

static void test_gpu_scanout(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    // Drive the full bring-up against the REAL device: resource + backing +
    // scanout, then one transfer+flush of a tiny rect. Every step returns
    // OK_NODATA (0x1100) from QEMU or gfx_setup/gfx_flush_rect report failure.
    static uint32_t pix[16];                  // a 4x4 dummy framebuffer
    KASSERT(gfx_setup((uint64_t)(uintptr_t)pix, 4, 4) == 0);
    KASSERT(gfx_flush_rect(0, 0, 4, 4) == 0);
}
```

- [ ] **Step 2: `make test` → red (link errors).**

- [ ] **Step 3: implement.** `src/gfx.h`:

```c
// gfx.h -- virtio-gpu 2D: a dumb scanout for a guest-built framebuffer.
#pragma once
#include <stdint.h>
void gfx_init(void);                     // find + reset + queues (kmain)
int  gfx_present(void);
uint32_t gfx_width(void);                // preferred scanout size (1280x720)
uint32_t gfx_height(void);
// Point the scanout at a guest framebuffer (physical, contiguous, w*h*4 bytes,
// BGRX little-endian = 0xXXRRGGBB words). Returns 0 on success.
int  gfx_setup(uint64_t fb_phys, uint32_t w, uint32_t h);
// Push one damage rect: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. 0 on success.
int  gfx_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
```

`src/virtio_gpu.c` — the command structs straight from the virtio spec
(§5.7): every request begins with a `ctrl_hdr`; every response is read back
into a second, device-writable buffer; `0x1100` = RESP_OK_NODATA.

```c
#define VIRTIO_ID_GPU 16
#define CMD_GET_DISPLAY_INFO   0x0100
#define CMD_RESOURCE_CREATE_2D 0x0101
#define CMD_SET_SCANOUT        0x0103
#define CMD_RESOURCE_FLUSH     0x0104
#define CMD_TRANSFER_TO_HOST   0x0105
#define CMD_ATTACH_BACKING     0x0106
#define RESP_OK_NODATA         0x1100
#define FORMAT_B8G8R8X8        2          // bytes B,G,R,X = LE word 0xXXRRGGBB

struct ctrl_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx, pad[3]; };
struct gpu_rect { uint32_t x, y, w, h; };
// ...one struct per command, then:
static int gpu_cmd(void *cmd, int cmd_len)   // submit cmd + response, check OK
{
    static struct ctrl_hdr resp;
    resp.type = 0;
    struct vbuf bufs[2] = {
        { (uint64_t)(uintptr_t)cmd,   (uint32_t)cmd_len, 0 },
        { (uint64_t)(uintptr_t)&resp, sizeof(resp),      1 },
    };
    virtq_submit(gpu_base, &ctlq, bufs, 2);   // polled, like virtio-blk
    return resp.type == RESP_OK_NODATA ? 0 : -1;
}
```

`gfx_setup`: RESOURCE_CREATE_2D (id 1, format, w, h) → ATTACH_BACKING (one
entry: fb_phys, w*h*4) → SET_SCANOUT (scanout 0, rect {0,0,w,h}, id 1).
`gfx_flush_rect`: TRANSFER_TO_HOST_2D (rect, offset = (y*W + x)*4) →
RESOURCE_FLUSH (rect). Width/height fixed 1280×720 (`GFX_W`/`GFX_H`).

Makefile: `QEMU_GPU := -device virtio-gpu-device` added to `QEMU_FLAGS`.
kmain (after input init): `gfx_init(); kprintf(gfx_present() ? ... : ...);`

- [ ] **Step 4: `make test` → green. Commit** `feat(gfx): virtio-gpu 2D driver -- resource/backing/scanout/flush`.

### Task 2: gfx_acquire / gfx_flush syscalls + /bin/gfxtest

**Files:** Modify `src/syscall.h` + `user/syscalls.h` (`SYS_GFX_ACQUIRE 41`, `SYS_GFX_FLUSH 42`), `src/syscall.c`, `user/ulib.h/.c`. Create `user/gfxtest.c`. Modify `Makefile` (PROGS += gfxtest), `src/initrd.c`. Test: KTEST syscall-level + end-to-end in Task 3.

- [ ] **Step 1: failing KTEST** (`syscall: gfx_acquire maps a framebuffer`) — worker thread with a user-ish AS is overkill; instead drive `do_syscall` from a worker that has fds and ASSERT the returned info: this syscall needs an address space, so the worker test uses `proc`-style setup. Simplest honest test: call the syscall from a worker WITHOUT an AS and assert it fails cleanly (-1), plus a kernel-level test that `gfx_fb_alloc()` (the helper the syscall uses) returns a contiguous, page-aligned buffer of the right size and registers it with the device:

```c
static void test_gfx_fb_alloc(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    uint64_t pa = gfx_fb_alloc();             // allocate + setup scanout
    KASSERT(pa != 0);
    KASSERT((pa & 0xFFF) == 0);               // page-aligned
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)pa;
    fb[0] = 0x00FF0000;                       // red pixel, top-left
    KASSERT(gfx_flush_rect(0, 0, 1, 1) == 0);
}
```

- [ ] **Step 2: red → implement.**
  - `gfx_fb_alloc()` in virtio_gpu.c: `pmm_alloc_pages(GFX_W*GFX_H*4/4096)`,
    zero it, `gfx_setup(pa, GFX_W, GFX_H)`, return pa (0 on failure; idempotent
    — second caller gets the same buffer).
  - `SYS_GFX_ACQUIRE` (x0 = `struct gfx_info*`): `pa = gfx_fb_alloc()`;
    build the page array; `va = as_map_phys(sched_current_as(), pages, n)`;
    fill `{ va, w, h, pitch }` into the user struct; 0/-1.
  - `SYS_GFX_FLUSH` (x0..x3 = x,y,w,h): clamp to the framebuffer, call
    `gfx_flush_rect`.
  - ulib: `struct gfx_info { void *fb; unsigned int w, h, pitch; };`
    `int gfx_acquire(struct gfx_info *gi);` `int gfx_flush(int x,int y,int w,int h);`

- [ ] **Step 3: `/bin/gfxtest`** — draws the proof pattern: red / green / blue
  vertical thirds plus a white 8×8 square at (8,8), flushes the whole frame,
  prints `gfxtest: pattern drawn`, sleeps forever (so the check can dump):

```c
#include "ulib.h"
int umain(void)
{
    struct gfx_info gi;
    if (gfx_acquire(&gi) != 0) { sys_write(1, "gfxtest: no gpu\n", 16); return 1; }
    unsigned int *fb = gi.fb;
    for (unsigned y = 0; y < gi.h; y++)
        for (unsigned x = 0; x < gi.w; x++)
            fb[y * (gi.pitch / 4) + x] =
                x < gi.w / 3 ? 0x00FF0000 : x < 2 * gi.w / 3 ? 0x0000FF00 : 0x000000FF;
    for (unsigned y = 8; y < 16; y++)
        for (unsigned x = 8; x < 16; x++) fb[y * (gi.pitch / 4) + x] = 0x00FFFFFF;
    gfx_flush(0, 0, gi.w, gi.h);
    sys_write(1, "gfxtest: pattern drawn\n", 23);
    for (;;) { sys_sleep(1000); }
}
```

- [ ] **Step 4: `make test` green. Commit** `feat(gfx): gfx_acquire/gfx_flush syscalls + /bin/gfxtest`.

### Task 3: screendump end-to-end check

**Files:** Modify `tools/lm_harness.py` (screendump helper + PPM parser). Create `tools/gfx_check.py`.

- [ ] **Step 1: harness:**

```python
def qmp_screendump(path: str):
    qmp("screendump", {"filename": path})

def ppm_pixel(path: str, x: int, y: int) -> tuple:
    """(r, g, b) at (x, y) of a binary P6 PPM (QEMU's screendump format)."""
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        f.readline()                          # maxval
        f.seek(3 * (y * w + x), 1)
        r, g, b = f.read(3)
        return (r, g, b)
```

- [ ] **Step 2: `tools/gfx_check.py`** — boot, `(run "gfxtest")`, wait for
  `pattern drawn`, screendump to a temp file, assert: (100,100) is red-ish,
  (640,100) green-ish, (1200,100) blue-ish, (12,12) white. PASS/FAIL prints.

- [ ] **Step 3: run it → PASS. Docs** (phase-25.md §25.2, README, roadmap ✅, drop the stale "graphics is deferred" README line). **Commit** `feat(gfx): screendump-verified userland framebuffer (Phase 25.2 done)`.
