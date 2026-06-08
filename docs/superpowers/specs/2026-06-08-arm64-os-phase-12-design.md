# MyOSv2 — Phase 12 Design (graphics: high-res framebuffer via ramfb)

**Date:** 2026-06-08
**Status:** Approved (autonomous build; roadmap pre-approved; resolution 1280×720)

## Goal

Put real, high-resolution pixels in a window. We allocate a **1280×720**
32-bit framebuffer in RAM, hand its address to QEMU's `ramfb` device (over the
**fw_cfg** interface), and then draw into it: a per-pixel gradient, filled
rectangles/color bars, and short text from a bitmap font. The shell keeps running
on the serial console; the graphics appear in a separate QEMU window.

This is a new **device class** (a display) and depends only on the MMU (Phase 6,
for MMIO + the identity-mapped framebuffer) and the PMM (Phase 4, to allocate the
buffer) — nothing in the process track.

## Why ramfb

The `virt` board has no built-in display. `-device ramfb` adds the simplest
possible one: a **guest-allocated linear framebuffer**. We pick the resolution,
allocate the memory ourselves, and tell QEMU "scan out *this* physical address,
this geometry, this pixel format." No PCI enumeration, no virtqueues — just one
configuration write through fw_cfg. It supports arbitrary resolution, so 1280×720
(or higher) is just a matter of the numbers we send.

## fw_cfg: the configuration channel (`fwcfg.c`)

fw_cfg is a tiny QEMU device for passing config blobs ("items") to the guest. On
`virt` its MMIO register block is at **`0x09020000`** (mapped device memory, so
already covered by our identity map):

| Offset | Width | Register |
|--------|-------|----------|
| `0x00` | 8 B | Data (legacy byte port) |
| `0x08` | 2 B | Selector / control |
| `0x10` | 8 B | DMA address |

**All fw_cfg multi-byte values are big-endian.** ARM is little-endian, so every
field we read or write gets byte-swapped (`bswap16/32/64` helpers).

### The DMA interface (what we use)

We drive everything through the DMA register. We place a control structure in RAM
and write its (big-endian) physical address to `0x10`; QEMU performs the transfer
synchronously and writes back the `control` field.

```c
struct FWCfgDmaAccess {   // all fields big-endian
    uint32_t control;     // bits below; high 16 bits = selector key when SELECT set
    uint32_t length;      // bytes to transfer
    uint64_t address;     // physical address of the data buffer
} __attribute__((packed));
```

Control bits: `ERROR=0x01`, `READ=0x02`, `SKIP=0x04`, `SELECT=0x08`, `WRITE=0x10`.

`fwcfg_dma(control, length, buf)` fills the struct (byte-swapped), writes
`bswap64(&dma)` to the DMA register, then polls `bswap32(dma.control)` until the
busy bits clear; a set `ERROR` bit means failure.

### Finding the `etc/ramfb` selector

Items are addressed by a 16-bit **selector key**. ramfb's key isn't fixed — we
look it up in the **file directory** (key `FW_CFG_FILE_DIR = 0x0019`):

```c
struct FWCfgFile {        // big-endian
    uint32_t size;
    uint16_t select;      // <- the key we want
    uint16_t reserved;
    char     name[56];    // e.g. "etc/ramfb"
};
// directory = uint32_t count (BE), then `count` FWCfgFile entries
```

`fwcfg_find_file("etc/ramfb")`:
1. DMA-read 4 bytes of the directory with `SELECT` (control `=(0x19<<16)|SELECT|READ`) → `count`.
2. DMA-read `count` × 64-byte entries (sequential, no SELECT); compare each
   `name`; return the entry's `select` key (and `size`), or -1 if absent.

If absent (e.g. the test build runs without `-device ramfb`), the caller skips
graphics gracefully — never hangs.

## The framebuffer (`fb.c`)

```c
struct fb_info { volatile uint32_t *pixels; uint32_t width, height, pitch_px; };
```

`fb_init()`:
1. Allocate the buffer: `pmm_alloc_pages(FB_PAGES)` where
   `FB_PAGES = 1280*720*4 / 4096 = 900` (3.6 MiB, page-exact). It's in
   identity-mapped Normal cacheable RAM, so the kernel writes pixels directly and
   QEMU reads the same physical bytes.
2. Find `etc/ramfb`; if missing, return 0 (no display) and leave `fb` unset.
3. Build the **RAMFBCfg** (big-endian) and DMA-**write** it to the ramfb selector:

```c
struct RAMFBCfg {         // all fields big-endian, packed (28 bytes)
    uint64_t addr;        // framebuffer physical address
    uint32_t fourcc;      // DRM_FORMAT_XRGB8888 = 0x34325258 ('XR24')
    uint32_t flags;       // 0
    uint32_t width;       // 1280
    uint32_t height;      // 720
    uint32_t stride;      // width * 4 = 5120
} __attribute__((packed));
```
   `ramfb_build_cfg(cfg, addr, w, h)` fills this (pure, unit-tested for byte
   layout); `fwcfg_dma((key<<16)|SELECT|WRITE, sizeof(cfg), &cfg)` registers it.
4. Store `fb_info` and return 1.

Pixel format `XRGB8888`: each pixel a `uint32_t` `0x00RRGGBB` (top byte unused).

## Drawing primitives (`draw.c`)

Pure pixel math over a caller-supplied buffer — fully unit-testable with a
`kmalloc`'d fake buffer (no hardware needed):

```c
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);             // -> 0x00RRGGBB
void draw_put(struct fb_info*, int x, int y, uint32_t c);  // bounds-checked
void draw_fill_rect(struct fb_info*, int x,int y,int w,int h, uint32_t c); // clipped
void draw_clear(struct fb_info*, uint32_t c);
void draw_char(struct fb_info*, int x,int y, char ch, uint32_t fg); // 8x16 glyph
void draw_text(struct fb_info*, int x,int y, const char* s, uint32_t fg);
```

`draw_put` ignores out-of-range coordinates; `draw_fill_rect` clips to the
framebuffer. Pixel offset is `y * pitch_px + x`.

### Font (`font8x8.h`)

An 8×8 bitmap font (one `uint8_t[8]` per glyph, MSB = leftmost column) covering
printable ASCII (`0x20`–`0x7E`); `draw_char` scales each glyph 2× to an effective
8×16 cell for legibility. `font_glyph(ch)` returns the 8-byte row array (a known
glyph's bytes are unit-tested). Scope: ASCII only, no kerning — extensible later.

## Run setup (`Makefile`)

`make test`/`make debug` stay byte-for-byte the same (`-display none`, no ramfb),
so the gate is unaffected. Only `make run` gains a window:

- Factor shared flags into `QEMU_BASE` (machine/cpu/mem/serial/kernel).
- `QEMU_FLAGS := $(QEMU_BASE) -display none` (test/debug — unchanged behavior).
- `run:` → `$(QEMU) $(QEMU_BASE) -display cocoa -device ramfb` — a macOS window
  plus the framebuffer device; the shell still runs on `-serial stdio`. Closing
  the window (or Ctrl-C on the terminal) quits QEMU.

## kmain integration

In the normal boot path (after `pmm_init`/`kheap_init`, before the scheduler), if
`fb_init()` succeeds, draw a demo frame: clear to dark, a full-screen gradient,
nested color bars (exercising `fill_rect`), and a `draw_text` banner
("MyOSv2  1280x720  framebuffer ok"). Guarded so a build without ramfb just logs
"no framebuffer (ramfb absent)" and continues to the shell.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/fwcfg.c`/`.h` | fw_cfg MMIO + DMA; `fwcfg_find_file`; `bswap` helpers |
| `src/fb.c`/`.h` | `fb_init` (alloc + ramfb register), `ramfb_build_cfg`, `fb_info` |
| `src/draw.c`/`.h` | `rgb`, `draw_put/fill_rect/clear/char/text` |
| `src/font8x8.h` | bitmap font data + `font_glyph` |
| `src/mmu.c` | (only if needed) confirm framebuffer region is Normal cacheable |
| `src/kmain.c` | `fb_init` + demo frame in the normal boot path |
| `Makefile` | `QEMU_BASE`; graphical `run` target with `-device ramfb` |
| `src/tests.c` | framebuffer/draw/cfg/bswap/font tests (test-first) |
| `docs/notes/phase-12.md` | notes |

## Testing (test-first, deterministic — hardware-free)

Pixels on screen aren't unit-testable (and the test build has no display), so we
test the **machinery** against in-memory buffers; the visible frame is the manual
confirmation (same approach as the shell):

1. `test_bswap_roundtrip` — `bswap16/32/64` produce the correct byte-reversed
   values for known inputs.
2. `test_fb_geometry` — for 1280×720: `pitch_px == 1280`, `FB_PAGES == 900`,
   `stride bytes == 5120`.
3. `test_ramfb_cfg_layout` — `ramfb_build_cfg(&cfg, 0x40500000, 1280, 720)`
   yields big-endian fields: `addr == bswap64(0x40500000)`,
   `fourcc == bswap32(0x34325258)`, `width == bswap32(1280)`,
   `height == bswap32(720)`, `stride == bswap32(5120)`.
4. `test_draw_put` — on a fake `fb_info` over a `kmalloc` buffer, `draw_put(3,2,c)`
   sets exactly `pixels[2*pitch_px + 3]` and nothing else; out-of-bounds writes
   are ignored.
5. `test_draw_fill_rect_clips` — a rect straddling the right/bottom edge fills
   only in-bounds pixels (no overflow past the buffer).
6. `test_font_glyph_known` — `font_glyph(' ')` is all zeros; a known glyph (e.g.
   `'A'`) matches its expected 8 bytes.

## Success criteria

- 6 new tests pass (test-first); all prior tests stay green; the gate is intact.
- `make run` opens a 1280×720 window showing the gradient, color bars, and text
  banner, while the shell remains usable on the serial console. A build/run
  without ramfb logs the skip and boots to the shell normally.

## Out of scope

A framebuffer **console** (mirroring `kprintf`/the shell to the screen);
double-buffering / vsync; `virtio-gpu` (dynamic resolution, cursor — a later
upgrade once the virtio transport exists); image/PNG decoding; mouse/keyboard GUI
input; non-ASCII fonts.
