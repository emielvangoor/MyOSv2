# Interrupt-driven I/O — sleep/wakeup, tty, and NIC

## Why

The kernel used to *poll*: the shell read input with `while (uart_getc() < 0)
yield();`, pipes spun the same way, and the network stack busy-looped a `net_pump()`
until a reply arrived. Ctrl-C was sampled on the timer tick. That's the wrong
architecture — a CPU should sleep until a device has something to say. This work
moves console and network I/O to the canonical Unix model:

> **An interrupt never does the work and never spins — it wakes a sleeping thread.**

It was built in three phases, each test-gated.

## Phase 1 — sleep/wakeup (`sched_block` / `sched_wake`)

The V6 primitive. A thread calls `sched_block(chan)` to sleep on an arbitrary
*wait-channel* (any address used as a key); an interrupt calls `sched_wake(chan)`
to make every waiter runnable. A new `THREAD_BLOCKED` state means the timer never
spuriously wakes a channel sleeper (unlike a timed `sleep`). A posted signal also
unblocks it — the **EINTR** path. The wakeup is advisory, so callers re-check
their condition in a loop. `sched_wait_event(chan, ticks)` adds a deadline (sleep
on a channel *with* a timeout) for I/O that might never get a reply.

## Phase 2 — interrupt-driven tty (`console.c`)

The PL011 raises a receive interrupt per byte; `console_isr()` runs the **line
discipline** (`console_input`): Ctrl-C (0x03) becomes a `SIGINT` to the foreground
program, every other byte is queued and a reader blocked in `console_getc()` is
woken. `read(fd 0)` now **sleeps** (`sched_block`) instead of yield-spinning, and
returns EINTR if a signal arrives. The timer-tick Ctrl-C poll is gone.

**QEMU quirk:** its PL011 only raises the receive interrupt once the FIFO trigger
(2 bytes) is reached and never implements the receive-timeout interrupt — so a
lone keystroke wouldn't interrupt. The fix is to leave the **FIFO disabled** (a
1-byte trigger → an interrupt per byte). This only works because the reader now
*blocks* rather than polling the data register: nothing drains the byte before the
interrupt is delivered. (A burst pasted faster than the 1-byte buffer drains can
still overrun; ordinary typing is well within budget.)

## Phase 3 — interrupt-driven NIC (`virtio-net`)

The virtio-net receive interrupt (GIC id derived from the device's mmio slot,
`48 + slot` on QEMU `virt`) wakes the stack. `net_ping` / `net_resolve` /
`arp_resolve` send, then **sleep** on a wait channel until the NIC IRQ delivers a
frame, a signal arrives (EINTR), or a short timeout re-checks the wall-clock
deadline. The ISR stays tiny — acknowledge the device and `sched_wake()`; the
protocol processing (`net_recv` + dispatch, which may transmit replies) runs in
the woken thread. That's the **top-half / bottom-half** split: the hard IRQ just
wakes; the real work happens in a normal, interruptible context.

A `sched_irqs_live` flag (set by kmain right after `enable_irqs`) keeps the boot
self-tests polling — at that point there's no timer running to wake a sleeper, so
the live network tests fall back to a wall-clock-bounded poll.

## The payoff

`ping example.com` now loops while **sleeping** between probes, woken the instant
the reply arrives; **Ctrl-C stops it immediately** (exit 130) because the sleeping
ping is woken by the console's SIGINT and unwinds via EINTR — no busy-wait, no
keyboard polling, correct layering throughout.

## What still polls (honestly)

- **TX completion**: `virtq_submit` polls the transmit used-ring briefly (QEMU
  completes a send near-instantly). Making it interrupt-driven is a small future
  change.
- **virtio-blk**: the disk driver still polls its used ring. Same pattern would
  apply.
