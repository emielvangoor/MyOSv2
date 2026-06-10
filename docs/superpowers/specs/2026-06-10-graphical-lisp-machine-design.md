# MyOSv2 Graphical Lisp Machine — Design (Phase 25)

## Vision

Give the Phase-24 Lisp machine a screen, following the **Emacs architecture**
end to end. The display is one full-screen **frame** per Lisp VM, tiled into
**windows** by a Lisp-owned tree; every window shows a **buffer**; buffers are
text in v1 (REPL, shell, scratch) and gain a **surface** kind in v2 — pixel
canvases that graphical programs render into, the EXWM move made native.
A mouse cursor is part of the design from the start.

Multiple Lisp VMs can run at once, each a complete machine with its own image
and its own redisplay engine; the kernel multiplexes the hardware between them
(the **seat**, i.e. the virtual-terminal model) and they can be swapped on the
fly.

Principles carried over from Phase 24:

- The kernel grows only **device drivers and thin syscalls**, never policy.
- Everything portable is **KTEST-able C** (the `lm_core.c` dual-build trick).
- **Lisp owns all semantics**; C owns mechanism (layout, glyphs, pixels).
- **Interrupts over polling**; blocking reads, IRQ-driven devices.
- Each sub-phase boots, is verified, and is its own commit.

Decisions locked by the user: architecture A (redisplay inside `/bin/lisp`,
Emacs-literal); **text machine first** (surfaces are v2 but designed-for now);
**virtio-gpu now** (not a ramfb revival); multiple VMs with on-the-fly seat
switching.

## Prior art / references

- **GNU Emacs**: Lisp mutates buffer/window/face structures; the C redisplay
  engine (`xdisp.c`) makes the screen match. We mirror this split exactly.
- **EXWM**: external graphical programs appear as buffers in the Emacs window
  tree — our v2 surface buffers, without X11 underneath.
- **Linux virtual terminals**: the seat/switching model.
- **Phase 12 (removed, in git history `ebb9d56`)**: 8×16 bitmap font and
  fill/blit code worth reviving inside `rd_core`.

## Architecture

```
┌──────────────────── /bin/lisp  (one per VM, PID 1 is the first) ───────────┐
│  frame.l: event loop · keymaps · REPL-as-buffer       (Lisp — semantics)   │
│  lm_gfx.c: buffer/window/face/redisplay/read-event DEFUNs   (user-only C)  │
│  src/rd_core.c: layout · glyphs · faces · damage · render to backbuffer    │
│      (portable C, dual-built into kernel KTEST + user ELF, like lm_core)   │
│  backbuffer: plain process memory registered with the kernel               │
└──────────────┬─────────────────────────────────────────────▲──────────────┘
     gfx_acquire/gfx_flush(rects)/seat_switch          input_read() events
┌──────────────▼─────────────────────────────────────────────┴──────────────┐
│  kernel: seat.c (active-client mux) · virtio_gpu.c · virtio_input.c        │
│          (drivers reuse the Phase-19 virtio transport, IRQ-driven)         │
└────────────────────────────────────────────────────────────────────────────┘
```

The redisplay engine lives **inside** each Lisp process, sharing its address
space with the Lisp image — zero-copy reads of the same structures Lisp
mutates, no display protocol to invent. The kernel's only graphics knowledge
is "transfer these pixels, flush these rects, route these input events."

## Kernel layer (the complete list of kernel changes)

- **`src/virtio_gpu.c`** — virtio-gpu 2D driver on the existing transport:
  `RESOURCE_CREATE_2D`, `ATTACH_BACKING` (guest pages = the client's
  backbuffer), `SET_SCANOUT`, `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` for
  damage rects. Hardware cursor plane is a later nicety; v1 cursor is drawn
  by rd_core.
- **`src/virtio_input.c`** — two devices: keyboard + tablet (absolute
  pointer, so QEMU never grabs the host mouse). IRQ handlers push normalized
  events (key up/down with code, pointer x/y, buttons) into the seat's queue.
- **`src/seat.c`** — the multiplexer. A process calls `gfx_acquire(w,h,fmt)`
  to become a display client and register its backbuffer; exactly one client
  is **active**. Active client: `gfx_flush` reaches the device, `input_read`
  receives events (blocking, poll-able). Inactive: flushes are silent no-ops.
  `seat_switch(n)` — syscall, plus a kernel hotkey (Ctrl-Alt-Fn) handled in
  the input driver — flips ownership and posts a full-damage "redraw" event
  to the incoming client. A client's exit releases its seat (normal file
  refcount cleanup). Seat 0 is the serial console and never goes away.
- **Syscalls**: `gfx_acquire`, `gfx_flush(rects)`, `input_read`,
  `seat_switch`. Nothing else.

## rd_core — the redisplay engine

`src/rd_core.c` + `src/rd.h`: portable, no libc, no kernel headers, dual-built
exactly like `lm_core.c` (kernel build drives KTEST layout cases into a
capture grid; user build renders real pixels). Components:

- **Model structs** (shared with the Lisp side of the process):
  - *frame*: screen geometry, the window-tree root, echo-area line, face table.
  - *window tree*: binary splits (horizontal/vertical, ratio) with leaf
    windows holding: buffer pointer, scroll offset, point, modeline format.
  - *buffer*: gap-buffer text storage; per-span face ids; a `kind` field
    (TEXT now, SURFACE in v2) so surfaces slot in without surgery.
  - *faces*: small id-indexed table of fg/bg/bold.
- **Layout**: window-tree walk → pixel rects → character grids (rect ÷ 8×16
  glyph cell); visible-line layout with wrap or truncate; modeline
  composition per window; echo area last line.
- **Rendering**: monospace bitmap font (`rd_font.c`, revived from Phase 12),
  glyph blits with face colors, window borders, cursor-cell inversion,
  software mouse-cursor sprite saved/restored over the pixels it covers.
- **Damage**: window-level dirty tracking (a buffer edit dirties only the
  windows showing that buffer; tree changes dirty the frame), emitting a
  minimal rect list for `gfx_flush`; seat-switch-in triggers full redraw.
- **Contract**: Lisp mutates structures and calls `redisplay()`; rd_core makes
  the screen match, touching only what changed. rd_core never calls Lisp.

## Lisp layer

- **`user/lm_gfx.c`** (user-only DEFUNs, the `lm_sys.c` pattern):
  - buffers: `(make-buffer name)`, `(insert str)`, `(delete-region a b)`,
    `(buffer-string)`, `(point)`, `(goto-char n)`, `(current-buffer)`,
    `(set-window-buffer w b)`.
  - windows: `(split-right)`, `(split-below)`, `(delete-window)`,
    `(other-window)`, `(selected-window)`.
  - display: `(set-face id :fg :bg :bold)`, `(redisplay)`.
  - input: `(read-event)` — blocking; returns events as Lisp data, e.g.
    `(key 120 down)`, `(mouse 1 412 300)`, `(redraw)`.
- **`user/lisp/frame.l`** — the machine's personality, all in Lisp:
  - the event loop: `(while t (dispatch (read-event)))`.
  - keymaps as alists: `C-x 2` split, `C-x o` other-window, `C-x b` switch
    buffer, printable keys self-insert; mouse-1 selects the clicked window.
  - the REPL as a buffer: line editing is buffer editing; `RET` evals the
    line, output inserts into the buffer.
  - dispatch: `lisp -frame` boots the graphical machine; plain `lisp` and
    `lisp -serve` behave as today (serial REPL stays the fallback).

## Multiple VMs and seats

`init` (PID 1) runs `lisp -frame`, acquires seat 1. `(spawn-vm)` forks+execs
another `lisp -frame`, which acquires seat 2 — a complete independent machine.
Ctrl-Alt-F2 or `(switch-seat 2)` flips the screen and input routing; switching
back finds the first VM exactly as it was (its image never stopped). Because
each VM renders into its own process memory, there is no shared device memory
to arbitrate: the seat only decides whose transfers/flushes the kernel honors.

## Surface buffers (v2, designed-for in v1)

`(make-surface-buffer name w h)` backs a buffer with a Phase-15 shared-memory
pixel canvas. In-image Lisp draws via surface primitives
(`surface-fill-rect`, `surface-blit`, …); an external ELF gets the shm handle
from `(run-in-buffer buf "plotdemo")` and renders into it — a tiny
Wayland-client-shaped contract, but appearing as a buffer in the window tree
(EXWM, native). rd_core blits the canvas into the window rect and composes
the modeline over it; damage = the canvas rect when the program signals it.
No OpenGL: honest software pixels (virtio-gpu 3D/acceleration is a possible
far-future phase). The only v1 obligation is the buffer `kind` field.

## Testing

- **KTEST (red→green, gating commits via `make test`)**:
  - rd_core layout into a capture grid: split rects, wrap/truncate, modeline
    text, face spans, cursor cell.
  - damage minimality: editing a buffer shown in one of two windows dirties
    only that window's rects.
  - gap-buffer ops: insert/delete/iterate across the gap.
  - virtio-gpu and virtio-input at the virtqueue level (command/event
    encode-decode), like the blk/net driver tests.
- **Boot-and-observe (`tools/`, the Phase-24 harness)**: the harness gains a
  QMP socket for QEMU's `screendump`; checks drive the REPL (serial or TCP)
  and assert on pixels — e.g. `(insert "hello")` changes the expected glyph
  row, `(split-right)` draws a border, seat-switch shows the other VM's
  distinct frame. Each sub-phase gets a check script.

## Phasing (each its own commit; the system always boots)

- **25.1 — virtio-input**: both devices + event queue + `input_read`;
  verified by echoing events to serial.
- **25.2 — virtio-gpu**: driver + `gfx_acquire`/`gfx_flush`; a userland test
  pattern proves pixels end to end.
- **25.3 — rd_core**: model structs, layout, font, damage — KTEST-first;
  live proof: a hardcoded two-window frame renders.
- **25.4 — Lisp integration**: `lm_gfx.c` + `frame.l`; `lisp -frame` boots
  the graphical machine; REPL-as-buffer works on screen.
- **25.5 — the seat**: `seat.c`, multi-VM, `(spawn-vm)`, hotkey + syscall
  switching.
- **25.6 — surface buffers**: shm canvases, surface primitives,
  `(run-in-buffer ...)` running an external renderer.

## Out of scope

OpenGL / GPU acceleration; overlapping windows; ttf/scalable fonts (bitmap
8×16 only); copy/paste between VMs; remote display. Graphics over the network
REPL is unaffected — `lisp -serve` keeps working on every VM.
