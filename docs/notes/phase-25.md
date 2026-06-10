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
