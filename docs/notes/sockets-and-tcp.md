# Sockets and a minimal TCP client

This closes out Phase 22's original scope ("TCP/IP stack + sockets"): a BSD-style
socket API over the existing UDP layer, then a minimal TCP client, both reachable
from user programs and verified against the real internet through QEMU user-net.

## Sockets in the fd table

A socket is just another kind of open file. `struct file` already distinguished a
vnode from a pipe; it now also carries a `struct socket *sock`. `socket()` makes a
socket object and installs a `struct file{sock}` in the fd table, so `close()`
(refcounted, fork-safe) frees it like any other handle.

The syscalls: `socket`, `bind`, `sendto`, `recvfrom` (UDP), plus `connect` and
plain `read`/`write` (TCP). `sendto`/`recvfrom` need five arguments, so ulib gains
a `syscall5` (the kernel already saves x0–x5 on the trap frame).

## UDP sockets

Each datagram socket holds a local port and a FIFO of received datagrams. The UDP
input path demuxes an arriving datagram to the socket bound to its destination
port and queues it. `recvfrom` is **interrupt-driven**: it pumps the stack and
sleeps on the NIC wait-channel until a datagram lands (or a signal arrives →
EINTR) — no busy-wait. `/bin/dnsq` proves it: it builds a DNS query in user space,
`sendto`s the resolver, `recvfrom`s the reply, and prints the address.

## Minimal TCP client (`tcp.c`)

Enough TCP to be a *client* on a reliable, in-order path (which QEMU user-net is):

- **Handshake** — `connect` sends SYN, waits for SYN-ACK (retransmitting the SYN
  on a timeout), then ACKs → ESTABLISHED.
- **Data** — `write` sends a PSH/ACK segment and waits for it to be acknowledged,
  retransmitting if needed. `read` accepts **in-order** segments into a ring,
  advances `rcv_nxt`, and ACKs; it returns 0 at EOF (peer FIN, ring drained).
- **Close** — `close` sends FIN and waits for the peer's FIN/ACK.
- **Checksum** — the internet checksum over the IPv4 **pseudo-header** + segment
  (a unit test confirms a filled-in header verifies back to 0).

Sequence numbers are 32-bit and wrap; comparisons use signed differences
(`(int32_t)(a-b)`) so they stay correct across the wrap.

### Corners deliberately cut

This is a *client*, not a stack: no out-of-order reassembly (in-order only), a
fixed advertised window, no listen/accept, no Nagle/delayed-ACK/congestion
control, and a best-effort retransmit rather than proper RTO estimation. Those are
exactly the parts a real TCP must get right — noted, not pretended.

### Verified

`/bin/http example.com` resolves the name, `connect`s to :80, `write`s
`GET / HTTP/1.0`, `read`s until the server closes, and prints the response —
`HTTP/1.1 200 OK`, ~800 bytes — a real TCP conversation with a real web server.

## Interrupt-driven throughout

Both `recvfrom` and `tcp_recv` follow the same pattern as the rest of the stack:
process whatever frames have arrived (`net_pump`), and otherwise **sleep** on the
NIC interrupt rather than spin. A pending signal aborts the wait (EINTR), so
Ctrl-C interrupts a blocked network read.
