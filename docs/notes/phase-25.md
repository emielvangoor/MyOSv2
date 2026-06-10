# Phase 25 — The graphical Lisp machine

Phase 25 gives the Phase-24 Lisp machine a screen, following the Emacs
architecture end to end. Design spec:
`docs/superpowers/specs/2026-06-10-graphical-lisp-machine-design.md`.
Plans live per sub-phase in `docs/superpowers/plans/`.

## 25.1 — virtio-input (keyboard + tablet)

The machine's first sensory organs. Two virtio-mmio devices share DeviceID 18
(their QEMU command-line order makes device 0 the keyboard, 1 the tablet), so
the transport gained `virtio_find_nth`. Each device's eventq is pre-posted
with 8-byte buffers; **the wire format is the Linux evdev triple**
`(type, code, value)` and we keep it end to end — device → kernel →
`input_read` syscall → userland all speak the same 8 bytes.

The interrupt split mirrors `virtio_net.c`: the ISR only acknowledges and
wakes (`top half`); the blocking `input_read` syscall drains used rings and
reposts buffers (`bottom half`), so an event storm never does work in IRQ
context. A pending signal aborts the wait (EINTR) — Ctrl-C stops a reader.

Why a **tablet** and not a mouse: it reports absolute coordinates (0..32767
per axis), so QEMU's window never grabs the host pointer and the guest cursor
can't drift out of sync.

`/bin/evtest` prints every event as `EV <type> <code> <value>` — the
human-readable probe.

Testing:
- KTEST: device enumeration, driver presence, the drain path (a used-ring
  inject hook plays the device's role), and the full syscall path.
- `tools/input_check.py`: boots, runs evtest, injects key + tablet events
  via **QMP** (`input-send-event` over a UNIX socket the harness now opens) —
  the same path a human's keys take into the graphical window — and asserts
  the events appear on the serial console.

## 25.2 — virtio-gpu (the display)

virtio-gpu in 2D mode is delightfully dumb — the guest renders into its OWN
RAM and the device scans it out. Bring-up is three control-queue commands
(RESOURCE_CREATE_2D → ATTACH_BACKING → SET_SCANOUT); steady state is two per
damage rect (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH), submitted polled like
virtio-blk's sector I/O (commands are rare or per-rect, never per-pixel).

`gfx_acquire` allocates the kernel framebuffer once — contiguous pages, so the
backing list is a single entry — and maps those same pages into the calling
process via the Phase-16 `as_map_phys` machinery; userland writes 0x00RRGGBB
words and calls `gfx_flush(x,y,w,h)`. `/bin/gfxtest` paints RGB thirds + a
white square.

**A bug worth remembering:** kmain runs the KTEST suite at every boot, then
resets the allocators for the real boot. `gfx_fb_alloc` cached its framebuffer
pointer in a static across that reset, so the real boot reused those pages for
page tables/stacks — and the first `(run "gfxtest")` painted red pixels over
kernel memory (FAR full of 0x00FF0000). Rule: **any driver static caching a
pmm-era pointer must be cleared by its init**; `gfx_init()` now forgets the
cached fb (KTEST `gpu: gfx_init forgets cached fb`). The exception handler now
prints FAR_EL1 alongside ELR/ESR — the pair named the culprit outright.

Verification: KTESTs drive the real device (incl. the learned-the-hard-way
minimum scanout size), and `tools/gfx_check.py` boots, runs gfxtest, takes a
**QMP screendump** and asserts exact pixel values at the pattern's corners.

## 25.3 — rd_core, the redisplay engine

The view half of the Emacs split, as portable dual-built C (`src/rd_core.c`,
the `lm_core` trick again): buffers are **gap buffers**, windows are a split
tree with a fixed node pool, faces are a small color table — and redisplay is
**glyph matrices** (what curses calls double buffering): layout the model into
a back grid of `{char, face}` cells, diff against the front grid, paint only
changed cells (recovered Phase-12 8×8 font, rows doubled → 8×16 cells, 160×45
at 1280×720), and emit the changed runs as damage rects for `gfx_flush`.
Damage minimality isn't an optimization pass — it falls out of the diff.

The cursor is the diff's one subtlety: a cell is also "changed" when the
cursor arrived or left it (painted inverted), and the cursor's last position
lives **in the frame struct**, not a static — the 25.2 cross-init lesson,
applied before it bit a second time.

KTESTs (all red→green): gap-buffer ops across the gap, single-window layout
(text, truncation, modeline face, echo row), split-below/right geometry,
damage confined to the edited window (and zero on a no-op redisplay), and
glyph pixels landing in a real framebuffer with face colors + inverted cursor.

## 25.4 — the Lisp integration: `lisp -frame`

The fusion: `/bin/lisp` now links BOTH cores (lm_core + rd_core), and
`user/lm_gfx.c` exposes the view to the language — buffers, windows, faces,
`(redisplay)`, `(read-event)` (raw evdev cooked into `(char N)` / `(ctrl N)` /
`(mouse X Y)`; what a key IS is C, what it DOES is Lisp), plus `(read-string)`
and `(prin1-to-string)` so a REPL can be built from parts. `user/lisp/frame.l`
is the personality: the event loop, the C-x keymap (2/3/0/o), mouse-1 window
selection, and the REPL as buffer editing — all redefinable from the REPL it
implements. `lisp -frame` boots it; errors unwind to C and re-enter the loop
with the image intact.

**The machine photographs itself:** `(screenshot "/shot.ppm")` dumps the
framebuffer as a PPM through ordinary file syscalls (per the user's idea that
the OS should be able to screenshot to memory/disk). First use found ramfs's
exact-fit growth (every append copied the whole file → ~1 GB of copying for
2.7 MB); ramfs now doubles capacity (amortized O(1) per byte) and frees the
old buffer instead of leaking it.

Also found live: a global `(setq flag nil)` read back as *unbound*, because
symbol value slots initialized to `Qnil` and lookup used `Qnil` to mean "no
binding". The core now has a `Qunbound` sentinel (KTEST `lm: global bound to
nil`).

Verification: `tools/frame_check.py` boots, starts the frame, **types on the
QMP keyboard** (`(+ 1 2)` RET, then the screenshot form), screendumps, and
**decodes the glyphs cell-by-cell against the same font the guest renders
with** — the check literally reads the screen. The captured frame lives at
`docs/images/phase-25-graphical-lisp-machine.png`.

## 25.5 — seats: multiple Lisp machines, swappable on the fly

The VT model: `src/seat.c` is pure bookkeeping (who owns the screen), KTESTed
as plain logic; the gpu driver grew per-seat resources (`gfx_resource_setup` /
`gfx_show`), so each VM renders into its OWN framebuffer and a switch is a
SET_SCANOUT — never a pixel copy. The active seat's `gfx_flush` reaches the
device; inactive VMs' flushes are silently dropped and their full frame is
replayed on switch-in. Input routes only to the active seat (inactive readers
sleep); **Ctrl-Alt-F1..F4** is consumed kernel-side in the input drain, and
`(switch-seat n)` does the same from Lisp. A dying VM's seat is released in
`thread_exit` and a survivor's frame takes the scanout. `(spawn-vm)` in
frame.l forks a fresh `lisp -frame` — a complete second machine.

Verification: `tools/seat_check.py` spawns VM2 from VM1, hotkey-switches, and
proves by glyph-decoded screendumps that VM2 evals `(* 6 7) → 42` on its own
virgin frame while VM1's history survives untouched on seat 1.

## 25.6 — surface buffers: the EXWM move

A buffer can now BE a pixel canvas: `(make-surface-buffer name w h)` backs it
with a Phase-16 shared-memory object (`SHM_PAGES_MAX` raised to 4 MiB for
canvases); `(surface-fill-rect ...)` draws from Lisp; `(run-in-buffer buf
"surftest")` fork+execs an external renderer, handing it the shm handle + 
geometry in argv — a tiny Wayland-client contract, except the "window" is an
Emacs buffer in the tree, with a modeline, switchable and splittable like any
other. rd_core blits the canvas over the window's text area each redisplay
(no diff for pixels — one blit is the honest price of arbitrary graphics).

**The fork+shared-memory bug this flushed out:** `as_clone` COW-demoted every
writable page — including `as_map_phys` mappings (shm canvases AND the display
framebuffer). After any fork, the parent's next framebuffer write went to a
private COW copy while the GPU kept scanning the original: a frozen screen.
`as_map_phys` mappings now carry a `PTE_SHARED` software bit and fork keeps
them writable + same-PA in both spaces (KTEST `vm: shared mapping survives
fork`). Known v1 blemish: stale text can ghost in a surface window's
off-canvas area in some sequences.

Verification: `tools/surface_check.py` — Lisp-drawn pixels, then surftest's
field/frame/square, all screendump-asserted inside the frame. Captured:
`docs/images/phase-25-surface-buffer.png`.

## Phase 25: done

The machine now looks like what it is: an Emacs frame as the screen, windows
as tiles, buffers as content — text or pixels, in-image Lisp or external
programs — multiple complete Lisp machines a Ctrl-Alt-Fn apart, every layer
verified by tests that literally read the glyphs off a screendump, and an OS
that can photograph itself.

## Post-25: beautiful text — the anti-aliased font renderer

The doubled 8×8 font gave way to **prerendered anti-aliased glyphs**:
`tools/gen_font.py` rasterizes a TTF on the host (currently **EmielPro**, the
machine owner's own Emacs font) into 12×24 cells of 8-bit coverage values,
committed as `src/font_aa.h` so builds never need the tool, the font file or
Python. At runtime `paint_cell` blends each pixel with integer math —
`out = bg + (fg−bg)·α/255` per channel — which is all grayscale font
anti-aliasing is; the expensive part (rasterizing curves) happened once, on
the host. No floating point anywhere (the FPU is still never enabled).

Grid changed from 160×45 to 106×30 cells. The KTESTs moved to AA-tolerant
assertions (pixels *closer to fg than bg*), and the boot-and-observe glyph
decoders in `tools/` now threshold against `font_aa.h` — the checks literally
read EmielPro off the screendump.

## Post-25: the GC stack-overflow incident

First sustained use of the graphical machine produced a banner suddenly
appearing mid-buffer (a frame-main restart), and the reproduction found worse:
`User data abort at 0x80000effb0` — a write ~3 MB below the user stack.
**`gc_mark` recursed once per cons**, so collecting a long list recursed as
deep as the list is long and blew through the 16-page stack mid-GC.

Fixes, all three:
- `gc_mark` now **iterates down cdr (and symbol-value) chains** and recurses
  only into cars: O(1) stack in list length, O(nesting) in tree depth.
- User stacks grew 16 → 64 pages (256 KiB) — a recursive-evaluator Lisp
  deserves headroom.
- The `-frame` restart loop checks `getpid()`: a forked pipeline child that
  errors now dies instead of unwinding into frame-main and scribbling its
  banner over the SHARED framebuffer.

Also: the font is now **Zed Mono** at 10×20 (the generator takes cell size on
the command line), and the geometry-dependent KTESTs/checks compute from the
cell constants, so font swaps no longer touch them.

## Post-25: M-x and describe-function — vertico-style

The minibuffer is a grown echo area: `rd_core` lets the echo hold multiple
lines (windows shrink above it) with one line rendered as a face-2 selection
bar — that's the entire engine cost of a vertico UI (KTEST `rd: minibuffer
echo + selection`). Everything else is Lisp + three introspection primitives:

- `(function-info 'sym)` → `(lambda PARAMS BODY)` / `(macro ...)` /
  `(primitive name min max)` — the LIVING object, not docs about it;
- `(all-symbols)` — the symbol table IS the command palette;
- `(string-search)` / `(substring)` for narrowing.

`M-x` (the cooker now tracks Meta; arrows cook to C-n/C-p) opens a
live-narrowing candidate list over every fbound symbol: type to filter,
C-n/C-p/arrows to move the bar, TAB adopts the selection, RET runs it, C-g
cancels. `C-h f` opens the same picker and shows the function in a `*Help*`
window — for a defun that's the actual lambda the machine runs, printed from
the image, ready to be redefined. Verified by `tools/mx_check.py` (narrowing,
execution, and the *Help* source read off screendumps); captured at
`docs/images/phase-25-mx-vertico.png`.
