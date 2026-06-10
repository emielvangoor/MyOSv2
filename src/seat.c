// seat.c -- the display/input multiplexer (Phase 25.5). See seat.h.
// ==================================================================
// Pure bookkeeping, deliberately: WHO owns the screen is decided here; the
// mechanics of re-pointing the scanout (virtio_gpu) and routing input
// (syscall.c) consult this table. Keeping it free of device and scheduler
// calls is what makes it trivially KTEST-able.

#include "seat.h"

struct seat {
    int used;
    int pid;
    uint64_t fb_pa;
};
static struct seat seats[SEAT_MAX];
static int active;                  // 1-based; 0 = no client yet

void seat_reset(void)
{
    for (int i = 0; i < SEAT_MAX; i++) { seats[i].used = 0; }
    active = 0;
}

int seat_register(int pid, uint64_t fb_pa)
{
    // One seat per process: re-acquiring returns the seat you already own
    // (gfx_acquire is idempotent and this keeps it that way).
    for (int i = 0; i < SEAT_MAX; i++) {
        if (seats[i].used && seats[i].pid == pid) { return i + 1; }
    }
    for (int i = 0; i < SEAT_MAX; i++) {
        if (!seats[i].used) {
            seats[i].used = 1;
            seats[i].pid = pid;
            seats[i].fb_pa = fb_pa;
            if (active == 0) { active = i + 1; }   // first client gets the screen
            return i + 1;
        }
    }
    return -1;
}

int seat_count(void)
{
    int n = 0;
    for (int i = 0; i < SEAT_MAX; i++) { n += seats[i].used; }
    return n;
}

int seat_active(void) { return active; }

int seat_active_pid(void)
{
    if (active == 0 || !seats[active - 1].used) { return -1; }
    return seats[active - 1].pid;
}

uint64_t seat_fb(int seat)
{
    if (seat < 1 || seat > SEAT_MAX || !seats[seat - 1].used) { return 0; }
    return seats[seat - 1].fb_pa;
}

int seat_switch(int n)
{
    if (n < 1 || n > SEAT_MAX || !seats[n - 1].used) { return -1; }
    active = n;
    return 0;
}

int seat_release_pid(int pid)
{
    for (int i = 0; i < SEAT_MAX; i++) {
        if (seats[i].used && seats[i].pid == pid) {
            seats[i].used = 0;
            if (active == i + 1) {
                active = 0;
                for (int j = 0; j < SEAT_MAX; j++) {
                    if (seats[j].used) { active = j + 1; break; }
                }
            }
            return active;
        }
    }
    return active;
}
