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
