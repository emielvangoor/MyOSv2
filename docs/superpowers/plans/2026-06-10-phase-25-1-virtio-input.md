# Phase 25.1 — virtio-input (keyboard + tablet) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** IRQ-driven virtio-input drivers (keyboard + absolute-pointer tablet) delivering evdev-style events to userland through a blocking `input_read` syscall, verified end-to-end by QMP-injected events.

**Architecture:** Two virtio-mmio input devices (DeviceID 18) reuse the Phase-19 transport. Each pre-posts 8-byte event buffers on its eventq; the ISR acks + wakes (top half), and the blocking syscall drains used rings and reposts buffers (bottom half) — the exact `virtio_net.c` pattern. Spec: `docs/superpowers/specs/2026-06-10-graphical-lisp-machine-design.md`.

**Tech Stack:** C (freestanding), virtio-mmio split virtqueues, KTEST, QEMU QMP for end-to-end injection.

---

### Task 1: QEMU input devices + finding the Nth virtio device

**Files:**
- Modify: `Makefile` (QEMU_FLAGS)
- Modify: `src/virtio.h`, `src/virtio.c`
- Test: `src/tests.c`

- [ ] **Step 1: Add the devices to QEMU_FLAGS** (shared by run *and* test, so KTEST sees them):

```make
# (in Makefile, after QEMU_SERIAL) Keyboard + absolute-pointer tablet for the
# graphical machine (Phase 25). The tablet reports absolute coordinates, so
# QEMU never grabs the host mouse.
QEMU_INPUT := -device virtio-keyboard-device -device virtio-tablet-device
QEMU_FLAGS := -machine virt -cpu cortex-a72 -m 256M -display none $(QEMU_SERIAL) \
              $(QEMU_INPUT) -kernel $(TARGET) $(QEMU_DISK)
```

- [ ] **Step 2: Write the failing KTEST** (in `src/tests.c`, near the net tests; registry entry `{ "input: two devices present", test_input_devices_present }`):

```c
// --- virtio-input (Phase 25.1) ---
static void test_input_devices_present(void)
{
    // The graphical machine needs BOTH a keyboard and a tablet. They are two
    // separate virtio devices with the same DeviceID (18), so the transport
    // needs to enumerate beyond the first match.
    KASSERT(virtio_find_nth(18, 0) != 0);
    KASSERT(virtio_find_nth(18, 1) != 0);
    KASSERT(virtio_find_nth(18, 2) == 0);   // there is no third one
}
```

- [ ] **Step 3: Run `make test`, confirm it fails** (link error: `virtio_find_nth` undefined).

- [ ] **Step 4: Implement `virtio_find_nth`** in `src/virtio.c` (generalize the existing scan; keep `virtio_find` as `virtio_find_nth(id, 0)`):

```c
// Find the MMIO base of the nth (0-based) virtio device with this DeviceID.
// Multiple identical devices are normal: virtio-input is one DeviceID (18)
// but appears twice (keyboard + tablet).
uint64_t virtio_find_nth(uint32_t device_id, int nth)
{
    for (uint64_t base = VIRTIO_MMIO_BASE;
         base < VIRTIO_MMIO_BASE + 32 * VIRTIO_MMIO_STRIDE;
         base += VIRTIO_MMIO_STRIDE) {
        if (rd(base, R_MAGIC) != 0x74726976) { continue; }
        if (rd(base, R_DEVICE_ID) != device_id) { continue; }
        if (nth-- == 0) { return base; }
    }
    return 0;
}

uint64_t virtio_find(uint32_t device_id) { return virtio_find_nth(device_id, 0); }
```

(Adapt constant names to the ones already in `virtio.c` — read the existing
`virtio_find` first and reuse its loop bounds/register helpers verbatim.)

Prototype in `src/virtio.h`:
```c
uint64_t virtio_find_nth(uint32_t device_id, int nth);
```

- [ ] **Step 5: `make test` → green. Commit** `feat(virtio): enumerate nth device with equal DeviceID + input devices in QEMU`.

### Task 2: the input driver (`src/virtio_input.c` + `src/input.h`)

**Files:**
- Create: `src/input.h`, `src/virtio_input.c`
- Modify: `src/kmain.c` (init + IRQ enable), `src/exceptions.c` (ISR dispatch)
- Test: `src/tests.c`

- [ ] **Step 1: Write `src/input.h`:**

```c
// input.h -- virtio-input keyboard + tablet -> evdev-style events (Phase 25.1).
#pragma once
#include <stdint.h>

// The wire format virtio-input uses IS the Linux evdev triple. We keep it.
struct input_event {
    uint16_t type;    // EV_KEY / EV_REL / EV_ABS / EV_SYN
    uint16_t code;    // KEY_*, BTN_*, ABS_X/ABS_Y
    uint32_t value;   // key: 1=down 0=up; abs: position 0..32767
};
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define ABS_X  0
#define ABS_Y  1

void input_init(void);                 // bring up both devices (kmain)
int  input_present(void);              // both keyboard and tablet found?
int  input_irq_id(int dev);            // GIC id per device (0=kbd, 1=tablet)
void input_isr(void);                  // ack both + wake readers
// Drain one event if available (NON-blocking). Returns 1/0. The blocking wait
// lives in the syscall layer (sched_wait_event on input_waitq()).
int  input_poll_event(struct input_event *out);
int *input_waitq(void);
```

- [ ] **Step 2: Write the failing KTESTs** (registry: `{ "input: driver present", test_input_present }`, `{ "input: poll drains an injected event", test_input_poll_drain }`):

```c
static void test_input_present(void)
{
    input_init();
    KASSERT(input_present());
}

static void test_input_poll_drain(void)
{
    // Fake a completed event in device 0's used ring, exactly as the device
    // would leave it, and check input_poll_event() hands it to us.
    input_init();
    struct input_event ev = { 0xFFFF, 0xFFFF, 0xFFFFFFFF };
    KASSERT(input_poll_event(&ev) == 0);            // nothing pending
    input_test_inject(0, 1 /*EV_KEY*/, 30 /*KEY_A*/, 1);
    KASSERT(input_poll_event(&ev) == 1);
    KASSERT(ev.type == 1 && ev.code == 30 && ev.value == 1);
    KASSERT(input_poll_event(&ev) == 0);            // consumed
}
```

`input_test_inject(dev, type, code, value)` is a small test hook in the driver
(writes an event into the dev's buffer and advances its used ring the way the
device would; compiled always — it is 6 lines and documents the ring layout).

- [ ] **Step 3: `make test` → both fail (link errors).**

- [ ] **Step 4: Implement `src/virtio_input.c`:**

```c
// virtio_input.c -- keyboard + tablet drivers (Phase 25.1).
// =========================================================
// Two virtio-mmio devices share DeviceID 18; device 0 is the keyboard and
// device 1 the tablet (their QEMU command-line order). Each has an eventq
// (queue 0) of 8-byte evdev triples. We pre-post EVN buffers per device; the
// device fills one per event. The ISR only acks + wakes (top half); the
// reader drains used rings and reposts buffers (bottom half) -- the same
// split virtio_net.c uses, so a storm of events never does work in IRQ context.
#include "input.h"
#include "virtio.h"
#include "sched.h"

#define VIRTIO_ID_INPUT 18
#define EVN 16

struct idev {
    uint64_t base;
    struct virtq q;
    struct input_event buf[EVN];
};
static struct idev devs[2];
static int ndev;
static int waitq;

static void post(struct idev *d, uint32_t id)
{
    d->q.desc[id].addr  = (uint64_t)(uintptr_t)&d->buf[id];
    d->q.desc[id].len   = sizeof(struct input_event);
    d->q.desc[id].flags = VIRTQ_DESC_F_WRITE;
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
        if (virtio_init(base) != 0) { break; }
        if (virtio_queue_init(base, &d->q, 0) != 0) { break; }  // eventq
        virtio_driver_ok(base);
        for (uint32_t i = 0; i < EVN && i < d->q.num; i++) { post(d, i); }
        *(volatile uint32_t *)(uintptr_t)(base + 0x050) = 0;    // notify eventq
        __asm__ volatile("dsb sy" ::: "memory");
        ndev = n + 1;
    }
}

int input_present(void) { return ndev == 2; }

int input_irq_id(int dev)
{
    if (dev >= ndev) { return -1; }
    return 48 + (int)((devs[dev].base - 0x0a000000UL) / 0x200);
}

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

int input_poll_event(struct input_event *out)
{
    for (int n = 0; n < ndev; n++) {
        struct idev *d = &devs[n];
        if (d->q.used[1] == d->q.last_used) { continue; }
        volatile uint32_t *u = (volatile uint32_t *)d->q.used;
        int slot = d->q.last_used % d->q.num;
        uint32_t id = u[1 + slot * 2];
        *out = d->buf[id];
        post(d, id);
        *(volatile uint32_t *)(uintptr_t)(d->base + 0x050) = 0;
        d->q.last_used++;
        return 1;
    }
    return 0;
}

// Test hook: pretend device `dev` delivered (type, code, value) -- fill the
// next buffer and advance the used ring exactly the way the hardware does.
void input_test_inject(int dev, uint16_t type, uint16_t code, uint32_t value)
{
    struct idev *d = &devs[dev];
    volatile uint32_t *u = (volatile uint32_t *)d->q.used;
    int slot = d->q.used[1] % d->q.num;
    uint32_t id = (uint32_t)(d->q.avail[2 + (slot % d->q.num)]);
    d->buf[id].type = type; d->buf[id].code = code; d->buf[id].value = value;
    u[1 + slot * 2] = id;
    u[2 + slot * 2] = sizeof(struct input_event);
    __asm__ volatile("dsb sy" ::: "memory");
    d->q.used[1] = (uint16_t)(d->q.used[1] + 1);
}
```

Declare the hook in `input.h`:
```c
void input_test_inject(int dev, uint16_t type, uint16_t code, uint32_t value);
```

- [ ] **Step 5: Wire init + IRQs.** In `src/kmain.c` next to `virtio_net_init()`:
```c
input_init();
if (input_present()) {
    gic_enable_irq(input_irq_id(0));    // keyboard
    gic_enable_irq(input_irq_id(1));    // tablet
}
```
In `src/exceptions.c`, next to the net dispatch:
```c
} else if ((int)id == input_irq_id(0) || (int)id == input_irq_id(1)) {
    input_isr();
```

- [ ] **Step 6: `make test` → green. Commit** `feat(input): virtio-input keyboard+tablet drivers, IRQ-driven (Phase 25.1)`.

### Task 3: the `input_read` syscall + /bin/evtest

**Files:**
- Modify: `user/syscalls.h` (`#define SYS_INPUT_READ 40`), `src/syscall.c`, `user/ulib.h`, `user/ulib.c`
- Create: `user/evtest.c`
- Modify: `Makefile` (PROGS += evtest), `src/initrd.c` (extern + add_prog)
- Test: `src/tests.c` (syscall-level), then end-to-end in Task 4

- [ ] **Step 1: Failing KTEST** (`{ "syscall: input_read drains event", test_syscall_input_read }`), same worker pattern as the fd tests:

```c
static volatile long inputread_res;
static void input_read_worker(void *a)
{
    (void)a;
    input_test_inject(0, EV_KEY, 30, 1);
    struct input_event ev;
    struct trapframe tf;
    tf.x[8] = SYS_INPUT_READ; tf.x[0] = (uint64_t)(uintptr_t)&ev;
    do_syscall(&tf);
    inputread_res = ((long)tf.x[0] == 0 && ev.type == EV_KEY &&
                     ev.code == 30 && ev.value == 1) ? 1 : -1;
}
static void test_syscall_input_read(void)
{
    pmm_init(); kheap_init();
    input_init();
    sched_init();
    inputread_res = 0;
    thread_create(input_read_worker, 0, 1);
    for (long i = 0; i < 100000 && !inputread_res; i++) { yield(); }
    KASSERT(inputread_res == 1);
}
```

- [ ] **Step 2: `make test` → fails.**

- [ ] **Step 3: Implement the syscall** in `src/syscall.c`:

```c
case SYS_INPUT_READ: {                    // x0 = struct input_event*  -> 0 / -1
    struct input_event *out = (struct input_event *)(uintptr_t)tf->x[0];
    if (!out) { ret = -1; break; }
    // Block (sleep/wake, never spin) until a device delivers an event.
    for (;;) {
        if (input_poll_event(out)) { ret = 0; break; }
        struct thread *t = sched_current();
        if (t && t->sig_pending) { ret = -1; break; }   // EINTR
        if (!sched_irqs_live()) { ret = -1; break; }    // test mode: don't sleep forever
        sched_wait_event(input_waitq(), 100);
    }
    break;
}
```
(`#include "input.h"` at the top of syscall.c.)

- [ ] **Step 4: ulib wrapper.** `user/ulib.h`:
```c
struct input_event { unsigned short type, code; unsigned int value; };
int input_read(struct input_event *ev);   // blocks; 0 on event, -1 on signal
```
`user/ulib.c` (copy the syscall-stub pattern used by `sys_read`):
```c
int input_read(struct input_event *ev) { return (int)syscall1(SYS_INPUT_READ, (long)ev); }
```
(Reuse the file's existing stub idiom — check how `sys_read` is written and
mirror it exactly, including the syscall macro/asm helper it uses.)

- [ ] **Step 5: `/bin/evtest`** (`user/evtest.c`):

```c
// evtest.c -- /bin/evtest: print input events until Ctrl-C. The Phase 25.1
// demo + integration-test probe: each event prints as "EV type code value".
#include "ulib.h"
static void put(const char *s) { sys_write(1, s, ustrlen(s)); }
static void putn(long v)
{
    char b[16]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}
int umain(void)
{
    put("evtest: waiting for events (Ctrl-C to stop)\n");
    struct input_event ev;
    while (input_read(&ev) == 0) {
        put("EV "); putn(ev.type); put(" "); putn(ev.code);
        put(" "); putn(ev.value); put("\n");
    }
    return 0;
}
```
Makefile: `PROGS := ... evtest`. `src/initrd.c`: extern `evtest_elf` + `add_prog("/bin/evtest", ...)`.

- [ ] **Step 6: `make test` → green. Commit** `feat(input): input_read syscall + /bin/evtest`.

### Task 4: QMP end-to-end check

**Files:**
- Modify: `tools/lm_harness.py` (QMP socket support)
- Create: `tools/input_check.py`

- [ ] **Step 1: Harness QMP support.** In `lm_harness.py`, add to `QEMU_CMD`:
```python
"-qmp", f"unix:{QMP_SOCK},server=on,wait=off",
```
with `QMP_SOCK = "/tmp/myosv2-qmp.sock"` and a helper:
```python
def qmp(cmd: str, args: dict | None = None) -> dict:
    """One QMP command over the UNIX socket (handshake each call -- simple
    beats stateful for a test harness)."""
    import json
    s = socket.socket(socket.AF_UNIX)
    s.connect(QMP_SOCK)
    f = s.makefile("rw")
    f.readline()                                  # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
    f.readline()
    f.write(json.dumps({"execute": cmd, "arguments": args or {}}) + "\n"); f.flush()
    resp = json.loads(f.readline())
    s.close()
    return resp

def qmp_key(qcode: str):
    for down in (True, False):
        qmp("input-send-event", {"events": [{"type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]})

def qmp_tablet(x: int, y: int):
    qmp("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": x}},
        {"type": "abs", "data": {"axis": "y", "value": y}}]})
```

- [ ] **Step 2: `tools/input_check.py`:**

```python
#!/usr/bin/env python3
"""Phase 25.1 end-to-end: QMP-injected keyboard + tablet events reach /bin/evtest."""
import sys, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_key, qmp_tablet

def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30): print("FAIL: no boot"); return 1
        q.send_line('(run "evtest")')
        if not q.expect(b"waiting for events", 10):
            print("FAIL: evtest did not start"); return 1
        time.sleep(0.3)
        qmp_key("a")                       # press+release 'a'
        # EV_KEY=1, KEY_A=30: expect "EV 1 30 1" (down) then "EV 1 30 0" (up)
        if not (q.expect(b"EV 1 30 1", 5) and q.expect(b"EV 1 30 0", 5)):
            print("FAIL: key events"); return 1
        print("ok: keyboard events arrive")
        qmp_tablet(16000, 8000)
        if not q.expect(b"EV 3 0 ", 5):    # EV_ABS=3, ABS_X=0
            print("FAIL: tablet events"); return 1
        print("ok: tablet events arrive")
        print("PASS: 25.1 virtio-input verified")
        return 0
    finally:
        q.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: `make all && python3 tools/input_check.py` → PASS.**

- [ ] **Step 4: Docs.** `docs/notes/phase-25.md` (new, 25.1 section), README capability bullet ("virtio-input"), ROADMAP Phase 25 section (25.1 ✅).

- [ ] **Step 5: `make test` green → Commit** `feat(input): QMP-driven end-to-end input check + docs (Phase 25.1 done)`.
