# Phase 21 notes — virtio-net driver

## What changed

The OS sends and receives raw **Ethernet frames** through a virtio-net NIC. Boot
logs the MAC, and a self-test does a real **ARP round-trip** with QEMU's gateway:

```
net: virtio-net 52:54:00:12:34:56 ready
...
[PASS] net: ARP round-trip       # who-has 10.0.2.2 -> gateway's ARP reply
```

## QEMU networking

QEMU's **user-mode** backend gives a virtual LAN with no host config — gateway
`10.0.2.2`, guest `10.0.2.15`, and a built-in ARP/ICMP/DHCP responder:
`-netdev user,id=net0 -device virtio-net-device,netdev=net0`. The gateway replies
to ARP, which is how the test proves both directions work. (This also sets up
`ping` for Phase 22.)

## Multi-queue transport

virtio-net needs two queues (0 = receive, 1 = transmit), so the Phase-19 transport
was split into `virtio_init` (reset + feature negotiation), `virtio_queue_init`
(set up one queue by index), and `virtio_driver_ok` (final). Each `struct virtq`
records its queue index so `virtq_submit` notifies the right one. `virtio-blk`
still uses the old one-shot `virtio_setup_queue` wrapper, unchanged.

## The driver (`virtio_net.c`)

Every frame on a queue is preceded by a **12-byte virtio-net header** (modern /
VERSION_1) we leave zero (no checksum/GSO offloads). On init we read the MAC from
config space (`base + 0x100`) and **pre-post 8 receive buffers** so the device
always has somewhere to put an incoming frame. `net_send` prepends the header and
submits on the TX queue (polling for completion); `net_recv` checks the RX used
ring, copies out the frame (minus the header), and recycles the buffer back onto
the RX available ring.

## A bug worth remembering

The ARP test first "failed" even though the driver was perfect — the received
frame *was* the ARP reply. The test checked the wrong ARP field: the reply's
**target** IP (offset 24, = our `10.0.2.15`) instead of its **sender** IP
(offset 14, = the gateway's `10.0.2.2`). A reminder that when a network test
fails, log the actual bytes before blaming the driver.

## Testing

2 tests, test-first, under `make test` with the NIC attached: the NIC is present
with a non-zero MAC; and a hand-built ARP request elicits the gateway's reply
(real TX + RX). The raw ARP bytes are assembled in the test — Phase 22 builds the
proper ARP layer on top.

## Limits

Polled RX (no IRQ); no control queue / multiqueue; no checksum/GSO offloads; no IP
stack yet (Phase 22). One TX buffer in flight at a time.
