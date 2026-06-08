// virtio.h -- generic virtio-mmio transport + a polled split virtqueue.
// =====================================================================
//
// virtio is the standard way a guest talks to paravirtual devices. The
// "transport" (here, memory-mapped I/O) carries control registers and a shared
// ring of buffers (the "virtqueue") that the guest and device take turns
// touching. We drive the queue by POLLING the device's "used" ring -- simple,
// and plenty for a block driver.
#pragma once
#include <stdint.h>

// One descriptor in the split virtqueue (a buffer the device should process).
struct virtq_desc {
    uint64_t addr;     // physical address of the buffer
    uint32_t len;      // length in bytes
    uint16_t flags;    // NEXT / WRITE (see below)
    uint16_t next;     // index of the next descriptor when NEXT is set
};
#define VIRTQ_DESC_F_NEXT  1   // this buffer chains to `next`
#define VIRTQ_DESC_F_WRITE 2   // the DEVICE writes this buffer (vs. reads it)

// Our handle on a set-up queue. avail/used point at the ring headers; we index
// past them by hand (the ring layouts are fixed by the spec).
struct virtq {
    volatile struct virtq_desc *desc;   // descriptor table (num entries)
    volatile uint16_t *avail;           // available ring: flags, idx, ring[num], ...
    volatile uint16_t *used;            // used ring:      flags, idx, {id,len}[num], ...
    uint32_t num;                       // queue size (entries)
    uint16_t last_used;                 // last used-ring index we consumed
};

// One buffer to submit (addr is physical; write = the device writes it).
struct vbuf { uint64_t addr; uint32_t len; int write; };

// Find the MMIO base of a virtio device with the given DeviceID (0 = none).
uint64_t virtio_find(uint32_t device_id);

// Reset, negotiate features, and set up queue 0 on a device. Returns 0 on success.
int virtio_setup_queue(uint64_t base, struct virtq *q);

// Submit a descriptor chain and POLL until the device finishes it. Returns 0.
int virtq_submit(uint64_t base, struct virtq *q, const struct vbuf *bufs, int n);
