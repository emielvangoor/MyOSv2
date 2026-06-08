# MyOSv2 — Phase 21 Design (virtio-net driver)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Get raw Ethernet frames in and out — the hardware half of networking. A
**virtio-net** driver on the **same virtio-mmio + virtqueue transport** built in
Phase 19, with receive (RX) and transmit (TX) queues and the device's MAC. This
is the foundation the TCP/IP stack (Phase 22) sits on.

Builds on the virtio transport (19).

## QEMU network backend

QEMU's **user-mode** networking gives the guest a virtual LAN with no host setup:
gateway `10.0.2.2`, guest `10.0.2.15`, and a built-in ARP/ICMP/DHCP responder.
Added to both `run` and `test`:
```
-netdev user,id=net0 -device virtio-net-device,netdev=net0
```
The gateway replies to ARP for `10.0.2.2`, so we can prove RX+TX with one
round-trip. (This also sets up `ping` to work in Phase 22.)

## Transport: multiple queues

virtio-net uses **two** queues — queue 0 = receive, queue 1 = transmit — so the
transport (Phase 19) is refactored to set up a queue *by index* between feature
negotiation and `DRIVER_OK`:
- `virtio_init(base)` — reset, ACK/DRIVER, negotiate `VIRTIO_F_VERSION_1`,
  `FEATURES_OK`.
- `virtio_queue_init(base, q, index)` — set up queue `index` (records the notify
  index in `struct virtq`).
- `virtio_driver_ok(base)` — final `DRIVER_OK`.
- `virtq_submit` notifies `q->notify_idx` (not a hard-coded 0).

`virtio_setup_queue` (used by virtio-blk) becomes a thin wrapper: init + queue 0 +
driver_ok — so the disk driver is unchanged.

## virtio-net (`virtio_net.c`, `net.h`)

Every frame on a virtqueue is preceded by a **virtio-net header** (12 bytes for
modern): flags/gso/checksum fields we leave zero (no offloads), and `num_buffers`
the device sets on RX.

- `virtio_net_init()`: `virtio_find(1)`; `virtio_init`; set up queue 0 (RX) and
  queue 1 (TX); `driver_ok`; read the 6-byte MAC from config space (`base+0x100`).
  **Pre-fill the RX queue** with receive buffers so the device has somewhere to
  put incoming frames.
- `net_send(frame, len)`: prepend a zeroed net header, submit on the TX queue,
  poll for completion.
- `net_recv(buf, max)`: poll the RX used ring; if a frame arrived, copy it out
  (skipping the net header), recycle the buffer back onto the RX queue, return the
  length. 0 if nothing waiting.
- `net_mac(out6)`, `net_present()`.

DMA buffers (headers + frame buffers) live in identity-mapped kernel RAM.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/virtio.h`/`virtio.c` | split into `virtio_init`/`virtio_queue_init`/`virtio_driver_ok`; per-queue notify |
| `src/virtio_net.c`, `src/net.h` | RX/TX queues, `net_send`/`net_recv`/`net_mac` |
| `src/kmain.c` | `virtio_net_init()` at boot; log the MAC |
| `Makefile` | QEMU `-netdev`/`-device virtio-net-device` (run + test) |
| `src/tests.c` | net tests (test-first) |
| `docs/notes/phase-21.md` | notes |

## Testing (test-first, runs under `make test` with the NIC attached)

1. `test_net_present` — after `virtio_net_init`, `net_present()` is true and the
   MAC is non-zero.
2. `test_net_arp_roundtrip` — hand-build a raw **ARP request** for `10.0.2.2`
   (Ethernet broadcast + ARP payload), `net_send` it, then poll `net_recv` for the
   gateway's **ARP reply** (opcode 2, sender IP `10.0.2.2`). This proves TX *and*
   RX against QEMU's responder. (The ARP bytes are hand-assembled here; Phase 22
   builds the real ARP layer.)

## Success criteria

- 2 net tests pass under `make test` (test-first); prior tests stay green; gate
  holds — a real frame goes out and a reply comes back.
- Live: boot logs the NIC's MAC; the disk and shell still work.

## Out of scope

IRQ-driven RX (we poll); the control queue / multiqueue; checksum/GSO offloads;
DHCP and the IP stack itself (Phase 22); link-status events.
