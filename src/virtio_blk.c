// virtio_blk.c -- a virtio block-device driver (read/write 512-byte sectors).
// ===========================================================================
//
// A block request is a 3-part descriptor chain: a header (what, where), the
// 512-byte data buffer, and a 1-byte status the device fills in. The data and
// status buffers must live in identity-mapped kernel RAM so their physical
// addresses equal their kernel pointers (the device DMAs to/from them); we use
// static buffers and bounce the caller's data through them.

#include <stdint.h>
#include "block.h"
#include "virtio.h"

#define VIRTIO_ID_BLOCK   2
#define VIRTIO_BLK_T_IN   0   // read:  device -> memory
#define VIRTIO_BLK_T_OUT  1   // write: memory -> device

struct blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static uint64_t blk_base;       // MMIO base of the device (0 = absent)
static struct virtq blk_q;
static int blk_ok;

// DMA staging buffers (in .bss == identity-mapped kernel RAM).
static struct blk_req_hdr req_hdr;
static uint8_t            req_data[BLOCK_SIZE];
static volatile uint8_t   req_status;

void virtio_blk_init(void)
{
    blk_ok = 0;
    blk_base = virtio_find(VIRTIO_ID_BLOCK);
    if (!blk_base) { return; }                       // no disk attached
    if (virtio_setup_queue(blk_base, &blk_q) != 0) { return; }
    blk_ok = 1;
}

int block_present(void) { return blk_ok; }

// One sector transfer. `write` selects direction; the data descriptor is marked
// device-writable only for a READ (the device fills our buffer then).
static int blk_rw(uint64_t sector, void *buf, int write)
{
    if (!blk_ok) { return -1; }

    req_hdr.type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req_hdr.reserved = 0;
    req_hdr.sector   = sector;
    if (write) {
        const uint8_t *s = buf;
        for (int i = 0; i < BLOCK_SIZE; i++) { req_data[i] = s[i]; }
    }
    req_status = 0xff;

    struct vbuf bufs[3] = {
        { (uint64_t)(uintptr_t)&req_hdr,    sizeof(req_hdr), 0 },          // device reads
        { (uint64_t)(uintptr_t)req_data,    BLOCK_SIZE,      write ? 0 : 1 }, // dev writes on read
        { (uint64_t)(uintptr_t)&req_status, 1,               1 },          // device writes
    };
    if (virtq_submit(blk_base, &blk_q, bufs, 3) != 0) { return -1; }
    if (req_status != 0) { return -1; }              // 0 = VIRTIO_BLK_S_OK

    if (!write) {
        uint8_t *d = buf;
        for (int i = 0; i < BLOCK_SIZE; i++) { d[i] = req_data[i]; }
    }
    return 0;
}

int block_read(uint64_t sector, void *buf)        { return blk_rw(sector, buf, 0); }
int block_write(uint64_t sector, const void *buf) { return blk_rw(sector, (void *)buf, 1); }
