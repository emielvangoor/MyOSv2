// virtio_net.c -- a virtio network driver: send/receive raw Ethernet frames.
// ==========================================================================
//
// Two virtqueues: queue 0 = receive, queue 1 = transmit. Every frame on a queue
// is preceded by a 12-byte virtio-net header (we leave it zero -- no offloads).
// We pre-post receive buffers so the device always has somewhere to deposit an
// incoming frame, and poll the used rings for completions.

#include <stdint.h>
#include "net.h"
#include "virtio.h"

#define VIRTIO_ID_NET 1
#define NET_HDR_LEN   12        // virtio-net header (modern / VERSION_1)
#define RXN           8         // receive buffers (== queue size)
#define BUFSZ         2048

static uint64_t     nic_base;
static struct virtq rxq, txq;
static int          nic_ok;
static uint8_t      mac[6];

// DMA buffers (identity-mapped kernel RAM): RX ring of buffers + one TX buffer.
static uint8_t rxbuf[RXN][BUFSZ];
static uint8_t txbuf[BUFSZ];

// Post receive buffer `id` (descriptor id) onto the RX available ring.
static void rx_post(uint32_t id)
{
    rxq.desc[id].addr  = (uint64_t)(uintptr_t)rxbuf[id];
    rxq.desc[id].len   = BUFSZ;
    rxq.desc[id].flags = VIRTQ_DESC_F_WRITE;     // the DEVICE writes the buffer
    rxq.desc[id].next  = 0;
    uint16_t aidx = rxq.avail[1];
    rxq.avail[2 + (aidx % rxq.num)] = (uint16_t)id;
    __asm__ volatile("dsb sy" ::: "memory");
    rxq.avail[1] = aidx + 1;
    __asm__ volatile("dsb sy" ::: "memory");
}

void virtio_net_init(void)
{
    nic_ok = 0;
    nic_base = virtio_find(VIRTIO_ID_NET);
    if (!nic_base) { return; }
    if (virtio_init(nic_base) != 0) { return; }
    if (virtio_queue_init(nic_base, &rxq, 0) != 0) { return; }   // receive
    if (virtio_queue_init(nic_base, &txq, 1) != 0) { return; }   // transmit
    virtio_driver_ok(nic_base);

    // MAC address lives at the start of the device config space (offset 0x100).
    volatile uint8_t *cfg = (volatile uint8_t *)(uintptr_t)(nic_base + 0x100);
    for (int i = 0; i < 6; i++) { mac[i] = cfg[i]; }

    // Hand all receive buffers to the device, then notify it.
    for (uint32_t i = 0; i < RXN; i++) { rx_post(i); }
    *(volatile uint32_t *)(uintptr_t)(nic_base + 0x050) = 0;     // QueueNotify queue 0
    __asm__ volatile("dsb sy" ::: "memory");

    nic_ok = 1;
}

int net_present(void) { return nic_ok; }
void net_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) { out[i] = mac[i]; } }

// The GIC interrupt id of this NIC. QEMU's 'virt' lays virtio-mmio transports out
// at 0x0a000000 + slot*0x200, with SPI (16 + slot) -> GIC id (48 + slot). We
// derive the slot from the base address virtio_find() gave us.
int net_irq_id(void)
{
    if (!nic_base) { return -1; }
    uint32_t slot = (uint32_t)((nic_base - 0x0a000000UL) / 0x200);
    return 48 + (int)slot;
}

// Acknowledge the device's interrupt: read InterruptStatus (0x60), write the same
// bits back to InterruptACK (0x64). Until this is done the line stays asserted.
void net_irq_ack(void)
{
    if (!nic_base) { return; }
    volatile uint32_t *istatus = (volatile uint32_t *)(uintptr_t)(nic_base + 0x60);
    volatile uint32_t *iack    = (volatile uint32_t *)(uintptr_t)(nic_base + 0x64);
    *iack = *istatus;
    __asm__ volatile("dsb sy" ::: "memory");
}

int net_send(const void *frame, int len)
{
    if (!nic_ok || len > BUFSZ - NET_HDR_LEN) { return -1; }
    for (int i = 0; i < NET_HDR_LEN; i++) { txbuf[i] = 0; }       // zero net header
    const uint8_t *f = frame;
    for (int i = 0; i < len; i++) { txbuf[NET_HDR_LEN + i] = f[i]; }

    struct vbuf b = { (uint64_t)(uintptr_t)txbuf, (uint32_t)(NET_HDR_LEN + len), 0 };
    return virtq_submit(nic_base, &txq, &b, 1);                   // device reads it
}

int net_recv(void *buf, int max)
{
    if (!nic_ok || rxq.used[1] == rxq.last_used) { return 0; }    // nothing arrived

    volatile uint32_t *u = (volatile uint32_t *)rxq.used;
    int slot = rxq.last_used % rxq.num;
    uint32_t id = u[1 + slot * 2];       // descriptor the device filled
    uint32_t dlen = u[2 + slot * 2];     // total bytes (net header + frame)

    int paylen = (int)dlen - NET_HDR_LEN;
    if (paylen < 0) { paylen = 0; }
    if (paylen > max) { paylen = max; }
    uint8_t *out = buf;
    for (int i = 0; i < paylen; i++) { out[i] = rxbuf[id][NET_HDR_LEN + i]; }

    rx_post(id);                          // recycle the buffer
    *(volatile uint32_t *)(uintptr_t)(nic_base + 0x050) = 0;      // notify queue 0
    rxq.last_used++;
    return paylen;
}
