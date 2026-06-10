// seat.h -- the display/input multiplexer (Phase 25.5): virtual terminals.
// ========================================================================
// Several Lisp VMs can each own a complete frame; the kernel decides whose
// pixels reach the scanout and who receives input -- exactly Linux's VT
// model. Each client renders into its OWN framebuffer (ordinary RAM), so
// "switching" is just re-pointing the scanout and replaying the newcomer's
// pixels; the inactive VMs keep running, their flushes silently dropped.
#pragma once
#include <stdint.h>

#define SEAT_MAX 4

void seat_reset(void);                       // forget all clients (boot/tests)
// Register the calling process as a display client. Returns the seat number
// (1-based), or -1 if the table is full. The first client becomes active.
int  seat_register(int pid, uint64_t fb_pa);
int  seat_count(void);
int  seat_active(void);                      // active seat number (0 = none)
int  seat_active_pid(void);                  // its owner (-1 = none)
uint64_t seat_fb(int seat);                  // a seat's framebuffer (0 = none)
// Switch to seat n (1-based): returns 0, or -1 if there is no such seat.
// The caller (syscall / hotkey) is responsible for re-pointing the scanout.
int  seat_switch(int n);
// Drop a client when its process exits, so the seat can be reused. If it was
// active, the lowest remaining seat takes over (returns its number, 0 if none).
int  seat_release_pid(int pid);
