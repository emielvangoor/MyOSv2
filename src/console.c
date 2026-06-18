// console.c -- terminal line discipline + interrupt-driven input.
// ===============================================================
//
// The Unix model: a device interrupt never does real work and never spins -- it
// hands a byte to the line discipline and WAKES a sleeping reader. Here:
//
//   UART RX interrupt --> console_isr() --> console_input(byte)
//        Ctrl-C (0x03)  --> SIGINT to the foreground program (not queued)
//        anything else  --> push to the input ring + sched_wake(readers)
//
//   read(fd 0) --> console_getc() --> sched_block() until the ISR wakes it
//                                      (or a signal arrives -> returns -1, EINTR)
//
// No polling: a blocked reader costs nothing until a key is actually pressed.

#include <stdint.h>
#include "console.h"
#include "uart.h"
#include "sched.h"
#include "signal.h"

// The input ring. The interrupt is the sole producer (advances `head`); a reader
// is the sole consumer (advances `tail`). Single producer + single consumer, so
// no lock is needed beyond masking IRQs around the consumer's check-and-block.
#define CBUF 256
static volatile unsigned char cbuf[CBUF];
static volatile unsigned int  chead, ctail;

// The wait-channel readers sleep on. Its address is just a key for sched_block/
// sched_wake -- the int itself is never read.
static int console_chan;

static void cpush(int c)
{
    unsigned int n = (chead + 1) % CBUF;
    if (n == ctail) { return; }            // ring full -> drop the byte
    cbuf[chead] = (unsigned char)c;
    chead = n;
}

static int cpop(void)
{
    if (ctail == chead) { return -1; }     // empty
    int c = cbuf[ctail];
    ctail = (ctail + 1) % CBUF;
    return c;
}

int console_has_input(void) { return chead != ctail; }

// The controlling terminal's FOREGROUND process group -- what Ctrl-C signals.
// A job-control shell (busybox ash) publishes its current foreground job's
// group here via tcsetpgrp() -> ioctl(TIOCSPGRP). 0 means "unset" -- fall back
// to the single foreground thread the scheduler tracks (the innermost wait4
// target), which is what makes Ctrl-C work even when no shell has claimed the
// terminal (e.g. a program run straight from the frame or the serial REPL).
static int g_fg_pgrp;
void tty_set_fg_pgrp(int pgrp) { g_fg_pgrp = pgrp; }
int  tty_fg_pgrp(void)         { return g_fg_pgrp; }

// Deliver SIGINT the way a tty INTR (Ctrl-C) does: to the terminal's foreground
// process GROUP if one is set (so a whole job dies, even when a shell put it in
// its own group), else to the single foreground thread. Shared by the serial
// line discipline (here) and the graphical keyboard (SYS_INPUT_READ).
void tty_intr(void)
{
    extern int sched_kill(int pid, int sig);
    if (g_fg_pgrp > 0 && sched_kill(-g_fg_pgrp, SIGINT) == 0) { return; }
    struct thread *fg = sched_foreground();
    if (fg) { signal_send(fg, SIGINT); }
}

// Line discipline for one received byte. Ctrl-C interrupts the foreground
// program; every other byte is queued for the reader.
void console_input(int c)
{
    if (c == 3) {                          // Ctrl-C (the INTR character)
        tty_intr();
        return;                            // consumed -- not echoed or queued
    }
    cpush(c);
    sched_wake(&console_chan);             // unblock a reader waiting for input
}

// The UART receive interrupt: drain every byte the device has, then acknowledge.
void console_isr(void)
{
    int c;
    while ((c = uart_rx_raw()) >= 0) { console_input(c); }
    uart_irq_ack();
}

// Block until one input byte is available, then return it. Returns -1 if a
// signal is posted while we wait (the EINTR convention) so the syscall can
// unwind and let the signal be delivered. IRQs are masked across the empty-check
// and the transition to BLOCKED so the ISR can't wake us between the two (a lost
// wakeup); sched_block() releases the CPU, and the ISR's sched_wake() resumes us.
int console_getc(void)
{
    uint64_t flags = irq_save();
    int c;
    while ((c = cpop()) < 0) {
        struct thread *t = sched_current();
        if (t && t->sig_pending) { irq_restore(flags); return -1; }  // EINTR
        sched_block(&console_chan);
    }
    irq_restore(flags);
    return c;
}
