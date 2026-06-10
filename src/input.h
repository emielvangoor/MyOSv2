// input.h -- virtio-input keyboard + tablet -> evdev-style events (Phase 25.1).
// ==============================================================================
// The first sensory organ of the graphical Lisp machine: key presses and
// absolute pointer positions arrive here as interrupts and leave as little
// (type, code, value) triples a process reads with the input_read syscall.
#pragma once
#include <stdint.h>

// The wire format virtio-input uses IS the Linux evdev triple, so we keep it
// end to end: device -> kernel -> userland all speak the same 8 bytes.
struct input_event {
    uint16_t type;    // EV_KEY / EV_REL / EV_ABS / EV_SYN
    uint16_t code;    // KEY_*, BTN_*, ABS_X / ABS_Y
    uint32_t value;   // key: 1=down 0=up (2=repeat); abs: position 0..32767
};
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define ABS_X  0
#define ABS_Y  1

void input_init(void);                 // bring up both devices (called by kmain)
int  input_present(void);              // both keyboard and tablet found?
int  input_irq_id(int dev);            // GIC id per device (0=kbd, 1=tablet)
void input_isr(void);                  // ack devices + wake readers (top half)
// Drain one event if available (NON-blocking; the bottom half). The blocking
// wait lives in the syscall layer: sched_wait_event on input_waitq().
int  input_poll_event(struct input_event *out);
int *input_waitq(void);

// Test hook: pretend device `dev` delivered (type, code, value) by advancing
// its used ring the way the hardware would. Lets KTEST exercise the drain
// path without a real keystroke.
void input_test_inject(int dev, uint16_t type, uint16_t code, uint32_t value);
