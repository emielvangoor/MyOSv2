// virtio.c -- generic virtio-mmio transport + a polled split virtqueue.
// =====================================================================

#include <stdint.h>
#include "virtio.h"
#include "pmm.h"

// --- virtio-mmio: 32 transport slots on the virt board, 0x200 apart. ---
#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200UL
#define VIRTIO_MMIO_COUNT  32

// Register offsets (a subset; modern / version 2).
#define R_MAGIC          0x000   // "virt" = 0x74726976
#define R_VERSION        0x004   // 2 = modern
#define R_DEVICE_ID      0x008   // 2 = block
#define R_DEV_FEAT       0x010
#define R_DEV_FEAT_SEL   0x014
#define R_DRV_FEAT       0x020
#define R_DRV_FEAT_SEL   0x024
#define R_QUEUE_SEL      0x030
#define R_QUEUE_NUM_MAX  0x034
#define R_QUEUE_NUM      0x038
#define R_QUEUE_READY    0x044
#define R_QUEUE_NOTIFY   0x050
#define R_STATUS         0x070
#define R_QUEUE_DESC_LO  0x080
#define R_QUEUE_DESC_HI  0x084
#define R_QUEUE_DRV_LO   0x090
#define R_QUEUE_DRV_HI   0x094
#define R_QUEUE_DEV_LO   0x0a0
#define R_QUEUE_DEV_HI   0x0a4

// Status bits.
#define S_ACKNOWLEDGE 1
#define S_DRIVER      2
#define S_DRIVER_OK   4
#define S_FEATURES_OK 8

#define VIRTIO_F_VERSION_1 32    // feature bit: modern (1.0) device

#define QUEUE_SIZE 8

static inline uint32_t rd(uint64_t base, uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void wr(uint64_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}
static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

// Find the MMIO base of the nth (0-based) virtio device with this DeviceID.
// Multiple identical devices are normal: virtio-input is one DeviceID (18)
// but appears twice on the command line (keyboard + tablet), and they keep
// their command-line order in the MMIO slot scan.
uint64_t virtio_find_nth(uint32_t device_id, int nth)
{
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (rd(base, R_MAGIC) != 0x74726976) { continue; }   // "virt"
        if (rd(base, R_VERSION) != 2) { continue; }          // modern only
        if (rd(base, R_DEVICE_ID) != device_id) { continue; }
        if (nth-- == 0) { return base; }
    }
    return 0;
}

uint64_t virtio_find(uint32_t device_id)
{
    return virtio_find_nth(device_id, 0);
}

int virtio_setup_queue(uint64_t base, struct virtq *q)
{
    if (virtio_init(base) != 0) { return -1; }
    if (virtio_queue_init(base, q, 0) != 0) { return -1; }
    virtio_driver_ok(base);
    return 0;
}

// Reset the device and negotiate features (accept only VIRTIO_F_VERSION_1).
// Leaves the device in the DRIVER + FEATURES_OK state, ready for queue setup.
int virtio_init(uint64_t base)
{
    wr(base, R_STATUS, 0);
    uint32_t status = S_ACKNOWLEDGE;
    wr(base, R_STATUS, status);
    status |= S_DRIVER;
    wr(base, R_STATUS, status);

    wr(base, R_DEV_FEAT_SEL, 1);  (void)rd(base, R_DEV_FEAT);   // feature bits 32..63
    wr(base, R_DRV_FEAT_SEL, 1);
    wr(base, R_DRV_FEAT, 1u << (VIRTIO_F_VERSION_1 - 32));
    wr(base, R_DRV_FEAT_SEL, 0);
    wr(base, R_DRV_FEAT, 0);
    status |= S_FEATURES_OK;
    wr(base, R_STATUS, status);
    if (!(rd(base, R_STATUS) & S_FEATURES_OK)) { return -1; }
    return 0;
}

// Set up queue `index`. The three rings each get a zeroed page (4 KiB aligns all
// of them; identity-mapped, so the physical address is the kernel pointer).
int virtio_queue_init(uint64_t base, struct virtq *q, int index)
{
    wr(base, R_QUEUE_SEL, (uint32_t)index);
    if (rd(base, R_QUEUE_NUM_MAX) < QUEUE_SIZE) { return -1; }
    wr(base, R_QUEUE_NUM, QUEUE_SIZE);

    uint64_t descp = (uint64_t)(uintptr_t)pmm_alloc();
    uint64_t availp = (uint64_t)(uintptr_t)pmm_alloc();
    uint64_t usedp = (uint64_t)(uintptr_t)pmm_alloc();
    for (int i = 0; i < 4096; i++) {
        ((uint8_t *)(uintptr_t)descp)[i] = 0;
        ((uint8_t *)(uintptr_t)availp)[i] = 0;
        ((uint8_t *)(uintptr_t)usedp)[i] = 0;
    }
    q->desc = (volatile struct virtq_desc *)(uintptr_t)descp;
    q->avail = (volatile uint16_t *)(uintptr_t)availp;
    q->used = (volatile uint16_t *)(uintptr_t)usedp;
    q->num = QUEUE_SIZE;
    q->last_used = 0;
    q->notify_idx = (uint32_t)index;

    wr(base, R_QUEUE_DESC_LO, (uint32_t)descp);
    wr(base, R_QUEUE_DESC_HI, (uint32_t)(descp >> 32));
    wr(base, R_QUEUE_DRV_LO, (uint32_t)availp);
    wr(base, R_QUEUE_DRV_HI, (uint32_t)(availp >> 32));
    wr(base, R_QUEUE_DEV_LO, (uint32_t)usedp);
    wr(base, R_QUEUE_DEV_HI, (uint32_t)(usedp >> 32));
    wr(base, R_QUEUE_READY, 1);
    return 0;
}

void virtio_driver_ok(uint64_t base)
{
    uint32_t status = rd(base, R_STATUS);
    wr(base, R_STATUS, status | S_DRIVER_OK);
}

// avail ring layout (uint16_t units): [0]=flags, [1]=idx, [2+]=ring[].
// used ring  layout: [0]=flags, [1]=idx, then {u32 id, u32 len} entries.

// Submit a descriptor chain and notify the device, but DON'T wait for it. The
// caller decides how to wait for completion -- by polling virtq_complete() (the
// block driver) or by sleeping until the device's interrupt (the NIC).
void virtq_kick(uint64_t base, struct virtq *q, const struct vbuf *bufs, int n)
{
    for (int i = 0; i < n; i++) {
        q->desc[i].addr  = bufs[i].addr;
        q->desc[i].len   = bufs[i].len;
        q->desc[i].flags = (uint16_t)((bufs[i].write ? VIRTQ_DESC_F_WRITE : 0) |
                                      (i < n - 1 ? VIRTQ_DESC_F_NEXT : 0));
        q->desc[i].next  = (uint16_t)(i + 1);
    }

    // Publish the head (descriptor 0) in the available ring, then notify.
    uint16_t idx = q->avail[1];
    q->avail[2 + (idx % q->num)] = 0;
    barrier();
    q->avail[1] = idx + 1;
    barrier();
    wr(base, R_QUEUE_NOTIFY, q->notify_idx);
    barrier();
}

// Has the device finished a submitted chain (the used ring advanced)? Consumes
// one completion. Returns 1 if a chain completed, 0 if still pending.
int virtq_complete(struct virtq *q)
{
    barrier();
    if (q->used[1] == q->last_used) { return 0; }
    q->last_used = q->used[1];
    return 1;
}

// Submit and POLL the used ring until the device finishes -- used by the block
// driver, where a read/write must complete before we return.
int virtq_submit(uint64_t base, struct virtq *q, const struct vbuf *bufs, int n)
{
    virtq_kick(base, q, bufs, n);
    while (!virtq_complete(q)) { barrier(); }
    return 0;
}
