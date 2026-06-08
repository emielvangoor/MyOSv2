# Phase 19 notes — virtio transport + virtio-blk

## What changed

The OS talks to a real (emulated) **disk**. A generic virtio-mmio + virtqueue
layer drives a **virtio-blk** device; the self-tests write a sector and read it
back through the full device path:

```
disk: virtio-blk ready
...
[PASS] block: write then read sector
[PASS] block: two sectors independent
```

This is the first device beyond the UART/timer, and the foundation for an on-disk
filesystem (next).

## virtio in three layers

**Transport (`virtio.c`)** — `virtio-mmio`. The `virt` board has 32 transport
slots at `0x0a000000`, `0x200` apart. `virtio_find(id)` scans them for the magic
value, version 2 (modern), and a matching DeviceID. Bring-up is a fixed dance:
reset → `ACKNOWLEDGE | DRIVER` → negotiate features (accept `VIRTIO_F_VERSION_1`,
bit 32) → `FEATURES_OK` → set up queue 0 → `DRIVER_OK`.

**Virtqueue (`virtio.c`)** — a *split* virtqueue: a descriptor table, an
**available** ring (driver → device), and a **used** ring (device → driver). Each
ring gets its own zeroed PMM page (identity-mapped, so its physical address is its
kernel pointer). `virtq_submit` writes a descriptor chain, publishes the head in
the available ring, bumps `avail->idx`, writes `QueueNotify`, then **polls**
`used->idx` until the device finishes — simple, no interrupts.

**Block driver (`virtio_blk.c`)** — a request is a 3-descriptor chain: a header
`{ type, _, sector }` the device reads, a 512-byte data buffer (device-writable
for a read, device-readable for a write), and a 1-byte status the device sets
(0 = OK). The DMA buffers are static (kernel `.bss`, identity-mapped); the
caller's data is bounced through them. `block_read`/`block_write` wrap it.

## Why it worked first try

The fiddly parts of virtio — and where bugs usually hide — are: splitting the
64-bit ring addresses into low/high MMIO writes, the memory **barriers** around
publishing `avail->idx` and reading `used->idx`, the descriptor **flags**
(`NEXT` chains them; `WRITE` means *device writes* the buffer), and keeping every
DMA buffer in identity-mapped RAM. Getting those right makes the round-trip just
work against QEMU.

## Testing

3 tests, test-first, run under `make test` with a 4 MiB `-drive` attached: the
disk probes present; a 512-byte pattern written to sector 1 reads back identical;
two sectors hold independent patterns. These are genuine device I/O through the
virtqueue, not mocks. The disk image is a real file, so writes also persist across
QEMU runs.

## Limits

Polled completion (no IRQ); one in-flight request; no real filesystem on the disk
yet (that's next); no partitions, flush/discard, indirect/packed rings, or
multiple devices. The same transport will carry virtio-net later.
