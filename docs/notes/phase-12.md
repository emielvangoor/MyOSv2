# Phase 12 notes — graphics: high-res framebuffer (ramfb)

## What changed

The kernel now brings up a **1280×720** 32-bit framebuffer and draws to it: a
full-screen gradient, nested `fill_rect` color bars, and a font-rendered banner.
`make run` opens a graphical window (the ramfb scanout) while the shell keeps
running on the serial console. Serial logs `gfx: framebuffer 1280x720 ready`.

A "framebuffer" is just a big pixel array in RAM (one `uint32_t` per pixel, row
by row). The display device re-reads it many times a second and paints a window,
so *drawing is only writing 0x00RRGGBB words into the right array slots*.

## Why ramfb (and not VGA / virtio-gpu)

The `virt` board has no built-in display. `-device ramfb` is the simplest one to
add: a **guest-allocated** linear framebuffer. We pick the resolution, allocate
the memory, and tell QEMU "scan out *this* physical address, this geometry, this
format" — no PCI enumeration, no virtqueues. `virtio-gpu` (dynamic resolution, a
cursor) is a later upgrade once the virtio transport exists.

## fw_cfg: the config channel (`fwcfg.c`)

ramfb is configured through **fw_cfg**, a tiny MMIO device at `0x09020000` on the
virt board that hands the guest named "items." We drive it entirely through its
**DMA interface**:

1. Build a control block in RAM — `{ control, length, address }`, **all
   big-endian** (ARM is little-endian, so every field is byte-swapped).
2. Write that block's physical address (also byte-swapped) to the DMA register at
   `+0x10`. QEMU performs the transfer synchronously and writes a status back
   into `control`; we spin until the busy bits clear.

`control` carries the item's 16-bit **selector key** in bits [31:16] plus
`SELECT` and `READ`/`WRITE` bits.

### Finding `etc/ramfb`

ramfb's selector isn't fixed; we look it up in the **file directory** (key
`0x0019`): read its 4-byte entry `count`, then read `count` × 64-byte entries
(each `{ size, select, reserved, name[56] }`, big-endian) and match
`name == "etc/ramfb"`. Absent (no `-device ramfb`, e.g. the headless test build)
→ `fb_init` returns 0 and the kernel boots on without a display. **It never
hangs.**

## Registering the framebuffer (`fb.c`)

`fb_init` allocates a contiguous run with `pmm_alloc_pages(FB_PAGES)` where
`FB_PAGES = 1280*720*4 / 4096 = 900` (3.6 MiB, **page-exact**). Because RAM is
identity-mapped, the physical address handed to QEMU is also a valid kernel
pointer to draw through. Then it DMA-**writes** the `RAMFBCfg` blob (big-endian:
`addr, fourcc='XR24', flags, width, height, stride`) to the ramfb selector, and
QEMU starts scanning out our pixels.

Pixel format **XRGB8888**: each pixel a `uint32_t` `0x00RRGGBB` (top byte unused).
`pitch_px == width`, so a pixel is at `pixels[y*pitch_px + x]`.

## Drawing (`draw.c`, `font8x8.h`)

Everything funnels through `draw_put`, which **clips to the framebuffer** — so
`fill_rect`/`clear`/text are automatically edge-safe. The font is the public-
domain **font8x8_basic** (8 bytes/glyph; bit order **LSB = leftmost column**, so
`draw_char` tests `byte & (1<<col)`), each glyph scaled 2× for legibility at this
resolution.

## Run setup

`make test`/`make debug` are unchanged (`-display none`, no ramfb) so the gate
stays headless and green. Only `make run` gained a window:
`-display cocoa -device ramfb`, with the shell still on `-serial stdio`. Close the
window or Ctrl-C the terminal to quit. Factored the shared flags into
`QEMU_BASE`.

## Testing

Pixels on screen aren't unit-testable (and the test build has no display), so the
6 tests pin the **machinery** against in-memory buffers — `bswap` round-trips, fb
geometry (`pitch=1280`, `FB_PAGES=900`, `stride=5120`), the `RAMFBCfg`
big-endian byte layout, `draw_put` hitting exactly one pixel, `fill_rect`
clipping at the edges, and font glyph lookup. The visible frame is the manual
confirmation (same pattern as the shell). For this phase the frame was captured
headlessly via QEMU's monitor `screendump` and verified: the top-left pixel
`(0,0x20,0)` matches the gradient formula exactly.

## Limits / out of scope

No framebuffer **console** (the shell stays on serial); no double-buffering or
vsync (we draw straight to the scanned-out buffer); cacheable framebuffer memory
relies on QEMU's cache-less DMA model (a real GPU would need a cache clean);
`virtio-gpu`, image decoding, and GUI input are later work.
