# MyOSv2 — Phase 19 Design (virtio transport + virtio-blk)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Talk to a real (emulated) disk. Build the generic **virtio-mmio + virtqueue**
machinery, then a **virtio-blk** driver that reads and writes 512-byte sectors —
the first device beyond the UART/timer, and the gateway to persistent storage.

Builds on the MMU/MMIO (6) and the PMM (4).

## virtio on the `virt` board

QEMU exposes 32 **virtio-mmio** transport slots at `0x0a000000`, each `0x200`
bytes apart. With `-device virtio-blk-device,drive=...` one slot becomes a block
device. We use **modern** virtio (version 2) and drive the queue by **polling**
the used ring (no IRQ), which keeps the driver small.

### MMIO registers (offsets from a slot base)

`MagicValue 0x00` (= `0x74726976` "virt"), `Version 0x04` (2), `DeviceID 0x08`
(2 = block), `Status 0x70`, device/driver feature select+bits
(`0x10/0x14`, `0x20/0x24`), and per-queue: `QueueSel 0x30`, `QueueNumMax 0x34`,
`QueueNum 0x38`, `QueueReady 0x44`, `QueueNotify 0x50`, and the ring addresses
`QueueDesc 0x80/0x84`, `QueueDriver 0x90/0x94`, `QueueDevice 0xa0/0xa4`.

### Bring-up (`virtio.c`)

1. Scan the 32 slots for `MagicValue` ok, `Version==2`, matching `DeviceID`.
2. Reset (`Status=0`), set `ACKNOWLEDGE | DRIVER`.
3. Negotiate features: accept `VIRTIO_F_VERSION_1` (bit 32, required for modern);
   set `FEATURES_OK` and confirm it stuck.
4. Set up **queue 0** (a split virtqueue): a descriptor table, an available ring,
   and a used ring (allocated from the PMM, identity-mapped so their physical
   addresses are their kernel pointers). Write `QueueNum`, the three ring
   addresses, then `QueueReady=1`.
5. Set `DRIVER_OK`.

### A request (`virtio.c`)

`virtq_submit(descs[], n)` writes a chain into the descriptor table, publishes the
head in the available ring, bumps `avail->idx`, writes `QueueNotify=0`, then
**polls** `used->idx` until it advances. (Single outstanding request — simple and
enough for a polled block driver.)

## virtio-blk (`virtio_blk.c`, `block.h`)

A block request is three descriptors:
- a **header** `{ uint32 type; uint32 reserved; uint64 sector; }` (read by device),
- a **512-byte data** buffer (device writes it for a read, reads it for a write),
- a **status** byte (device writes 0 = OK).

```c
int block_read(uint64_t sector, void *buf512);   // 0 ok, -1 error
int block_write(uint64_t sector, const void *buf512);
int block_present(void);                          // is a disk attached?
```
`type` is `VIRTIO_BLK_T_IN (0)` for read, `VIRTIO_BLK_T_OUT (1)` for write. The
data descriptor's `WRITE` flag is set for a read (device writes our buffer).

## Run/test setup (`Makefile`)

Create a small raw disk image and attach it (both `run` and `test`):
```
-global virtio-mmio.force-legacy=false
-drive file=build/disk.img,if=none,format=raw,id=hd0
-device virtio-blk-device,drive=hd0
```
A `build/disk.img` (a few MiB of zeros) is created by the build.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/virtio.h`/`virtio.c` | virtio-mmio probe, feature negotiation, virtqueue, `virtq_submit` |
| `src/virtio_blk.c`, `src/block.h` | `block_read`/`block_write`/`block_present` |
| `src/kmain.c` | `virtio_blk_init()` at boot |
| `Makefile` | disk image + QEMU `-drive`/`-device` (run + test) |
| `src/tests.c` | block round-trip test (test-first) |
| `docs/notes/phase-19.md` | notes |

## Testing (test-first, runs under `make test` with the disk attached)

1. `test_block_present` — after `virtio_blk_init`, `block_present()` is true.
2. `test_block_write_read` — write a known 512-byte pattern to sector 1, read it
   back into a different buffer, and compare byte-for-byte.
3. `test_block_two_sectors` — distinct patterns on sectors 2 and 3 don't bleed
   into each other.

(These are genuine device I/O, exercising the full virtqueue path.)

## Success criteria

- 3 block tests pass under `make test` (test-first); prior tests stay green; gate
  holds. The virtqueue round-trip works against QEMU's virtio-blk.
- Live: a boot log line reports the disk (capacity); optionally a shell `bd`
  builtin writes/reads a sector.

## Out of scope

IRQ-driven completion (we poll); multiple in-flight requests; a real filesystem
on the disk (Phase 20); partitions; the indirect/packed virtqueue formats;
discard/flush; multiple block devices.
