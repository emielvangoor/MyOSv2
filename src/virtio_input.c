// virtio_input.c -- keyboard + tablet drivers (Phase 25.1).
// ==========================================================
//
// Two virtio-mmio devices share DeviceID 18; device 0 is the keyboard and
// device 1 the tablet (their QEMU command-line order). Each has an eventq
// (queue 0) the device fills with 8-byte evdev triples, one buffer per event.
// A statusq (queue 1) exists for LEDs and the like; we don't use it.
//
// We pre-post EVN buffers per device so the device always has somewhere to put
// the next event. The ISR only acknowledges and wakes (the top half); the
// reader drains used rings and reposts buffers (the bottom half) -- the same
// split virtio_net.c uses, so a storm of events never does work in IRQ
// context, and a slow reader merely drops events at the device instead of
// wedging the kernel.

#include "input.h"
#include "virtio.h"
#include "sched.h"
#include "gfx.h"

#define VIRTIO_ID_INPUT 18
// 64 pre-posted event buffers per device: a key press+release is two events,
// and a busy tick (minibuffer filtering, a streaming command) can let several
// keystrokes queue up -- 16 buffers dropped keys under fast typing.
#define EVN 64

struct idev {
    uint64_t base;                  // MMIO base (0 = absent)
    struct virtq q;                 // the eventq
    struct input_event buf[EVN];    // DMA buffers the device writes into
};
static struct idev devs[2];
static int ndev;
static int waitq;                   // readers sleep here; the ISR wakes it

// Hand buffer `id` (back) to the device on the available ring.
static void post(struct idev *d, uint32_t id)
{
    d->q.desc[id].addr  = (uint64_t)(uintptr_t)&d->buf[id];
    d->q.desc[id].len   = sizeof(struct input_event);
    d->q.desc[id].flags = VIRTQ_DESC_F_WRITE;       // the DEVICE writes it
    d->q.desc[id].next  = 0;
    uint16_t aidx = d->q.avail[1];
    d->q.avail[2 + (aidx % d->q.num)] = (uint16_t)id;
    __asm__ volatile("dsb sy" ::: "memory");
    d->q.avail[1] = aidx + 1;
    __asm__ volatile("dsb sy" ::: "memory");
}

void input_init(void)
{
    ndev = 0;
    for (int n = 0; n < 2; n++) {
        uint64_t base = virtio_find_nth(VIRTIO_ID_INPUT, n);
        if (!base) { break; }
        struct idev *d = &devs[n];
        d->base = base;
        d->q.last_used = 0;
        if (virtio_init(base) != 0) { break; }
        if (virtio_queue_init(base, &d->q, 0) != 0) { break; }   // eventq
        virtio_driver_ok(base);
        for (uint32_t i = 0; i < EVN && i < d->q.num; i++) { post(d, i); }
        *(volatile uint32_t *)(uintptr_t)(base + 0x050) = 0;     // notify eventq
        __asm__ volatile("dsb sy" ::: "memory");
        ndev = n + 1;
    }
}

int input_present(void) { return ndev == 2; }

// GIC id: QEMU's virt board lays virtio-mmio transports out at
// 0x0a000000 + slot*0x200 with SPI (16 + slot) -> GIC id (48 + slot),
// exactly like net_irq_id().
int input_irq_id(int dev)
{
    if (dev < 0 || dev >= ndev) { return -1; }
    return 48 + (int)((devs[dev].base - 0x0a000000UL) / 0x200);
}

// Top half: acknowledge both devices (cheap; reading InterruptStatus on a
// device that didn't interrupt is harmless) and wake any blocked reader. All
// event processing happens in the woken reader.
void input_isr(void)
{
    for (int n = 0; n < ndev; n++) {
        volatile uint32_t *istatus = (volatile uint32_t *)(uintptr_t)(devs[n].base + 0x60);
        volatile uint32_t *iack    = (volatile uint32_t *)(uintptr_t)(devs[n].base + 0x64);
        *iack = *istatus;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    sched_wake(&waitq);
}

int *input_waitq(void) { return &waitq; }

// Bottom half: take one completed event off either device's used ring, copy it
// out, and recycle the buffer. Keyboard is checked before tablet -- with one
// event per buffer the per-device order is preserved, which is what matters.
int input_poll_event(struct input_event *out)
{
    for (int n = 0; n < ndev; n++) {
        struct idev *d = &devs[n];
        if (d->q.used[1] == d->q.last_used) { continue; }
        volatile uint32_t *u = (volatile uint32_t *)d->q.used;
        int slot = d->q.last_used % d->q.num;
        uint32_t id = u[1 + slot * 2];               // which buffer completed
        *out = d->buf[id];
        post(d, id);                                  // recycle it
        *(volatile uint32_t *)(uintptr_t)(d->base + 0x050) = 0;
        d->q.last_used++;
        // Tablet moves also steer the hardware cursor plane, right here in
        // the kernel: the pointer stays visible and smooth no matter what
        // userland is doing with the events.
        if (out->type == EV_ABS) {
            static int mx, my;
            if (out->code == ABS_X) { mx = (int)((uint64_t)out->value * GFX_W / 32768); }
            if (out->code == ABS_Y) { my = (int)((uint64_t)out->value * GFX_H / 32768); }
            gfx_cursor_move(mx, my);
        }
        return 1;
    }
    return 0;
}

// Test hook: play the device's role for one event. The device takes buffers
// from the available ring in FIFO order, so completion k uses avail entry k;
// we fill that buffer and publish it on the used ring.
void input_test_inject(int dev, uint16_t type, uint16_t code, uint32_t value)
{
    struct idev *d = &devs[dev];
    volatile uint32_t *u = (volatile uint32_t *)d->q.used;
    uint16_t uidx = d->q.used[1];
    uint32_t id = d->q.avail[2 + (uidx % d->q.num)];
    d->buf[id].type = type; d->buf[id].code = code; d->buf[id].value = value;
    int slot = uidx % d->q.num;
    u[1 + slot * 2] = id;
    u[2 + slot * 2] = sizeof(struct input_event);
    __asm__ volatile("dsb sy" ::: "memory");
    d->q.used[1] = (uint16_t)(uidx + 1);
}
