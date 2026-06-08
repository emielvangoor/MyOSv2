# MyOSv2 — Phase 22 Design (TCP/IP stack + sockets) — CAPSTONE

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

The OS speaks to the network: a real protocol stack on top of the virtio-net NIC,
and a **sockets API** user programs use. The headline deliverable is a
**user-space `/bin/ping`** that pings QEMU's gateway and prints the round-trip.
Built layer by layer — each its own testable milestone:

```
Ethernet/ARP -> IPv4/ICMP (ping) -> UDP (sockets) -> TCP (client)
```

Builds on virtio-net (21) and the fd table (8).

## Architecture: a kernel net thread

Frames arrive asynchronously, so a **kernel thread** drives the stack: it loops
`net_recv()` and dispatches each frame; when idle it `yield`s. Blocking callers
(ARP resolve, ping, `recvfrom`, `accept`) also `yield` while the net thread
processes incoming frames — which works on our cooperative scheduler. Stack state
is a single static: our IP `10.0.2.15`, gateway `10.0.2.2`, our MAC, an ARP cache.

## Layers

**Ethernet / ARP (`arp.c`).** Resolve an IP to a MAC and answer requests for our
IP. An ARP cache `{ip, mac}`; `arp_resolve(ip)` checks it, else broadcasts a
request and yield-polls until the net thread fills it (or times out). `arp_input`
caches replies and answers requests for `10.0.2.15`.

**IPv4 + ICMP (`ip.c`, `icmp.c`).** `ip_send(dst, proto, payload, len)` resolves
the next hop (the gateway unless `dst` is on-link), builds the IPv4 header (with
the one's-complement checksum), and transmits. `ip_input` verifies and demuxes by
protocol. ICMP echo: `net_ping(ip, &ms)` sends an echo request and waits for the
matching reply; `icmp_input` also answers echo requests (so the guest is
pingable). The internet checksum is a pure, unit-tested function.

**UDP + sockets (`udp.c`, `socket.c`).** A socket table; a UDP socket has a local
port and a small receive queue of datagrams. `udp_input` enqueues to the matching
socket. The **sockets API** (integrated with the fd table, so a socket is an fd):
```
socket(domain, type)        -> fd          (AF_INET, SOCK_DGRAM / SOCK_STREAM)
bind(fd, ip, port)          -> 0
sendto(fd, buf, len, ip, port)
recvfrom(fd, buf, len, *ip, *port)         (blocks)
connect / send / recv / listen / accept    (TCP, below)
close(fd)
```

**TCP (`tcp.c`).** A minimal client: the three-way handshake (`connect`),
in-order `send`/`recv` with sequence/ack numbers and retransmit-on-timeout, and
`close` (FIN). One connection's worth of state machine
(SYN_SENT → ESTABLISHED → FIN_WAIT → CLOSED). Enough to open a connection to a
host service and exchange a little data. (Full server-side `listen`/`accept`,
windowing, and congestion control are out of scope — noted below.)

## Syscalls

```
SYS_SOCKET   23   // x0=domain, x1=type -> fd
SYS_BIND     24   // x0=fd, x1=ip, x2=port
SYS_SENDTO   25   // x0=fd, x1=buf, x2=len, x3=ip, x4=port
SYS_RECVFROM 26   // x0=fd, x1=buf, x2=len, x3=&ip, x4=&port -> len
SYS_CONNECT  27   // x0=fd, x1=ip, x2=port (TCP)
SYS_PING     28   // x0=ip, x1=&ms -> 0/-1   (ICMP echo; for /bin/ping)
```
`send`/`recv`/`close` on a TCP socket reuse `SYS_WRITE`/`SYS_READ`/`SYS_CLOSE`
through the fd (a socket fd routes to the stack, like a pipe end does).

## User side + demo

`/bin/ping <addr>`: since there's no argv yet, `/bin/ping` pings the gateway
`10.0.2.2` via `SYS_PING` and prints the round-trip time, e.g.
`ping 10.0.2.2: reply in 0 ms`. A `/bin/udptest` (or kernel test) exercises a UDP
socket. The shell runs `ping` like any `/bin` program.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/net/eth.c`,`arp.c`,`ip.c`,`icmp.c`,`udp.c`,`tcp.c`,`socket.c` (or flat `net*.c`) | the stack |
| `src/net.h` (extend) | stack init, `net_ping`, socket calls |
| `src/sched.c` | the net kernel thread (started at boot) |
| `src/syscall.*` | socket/ping syscalls; socket-fd routing in read/write |
| `user/syscalls.h`, `user/ulib.*` | wrappers |
| `user/ping.c` (+ udptest) | demos |
| `Makefile`, `src/initrd.c` | build + register `/bin/ping` |
| `src/tests.c` | checksum + ARP + ICMP + UDP tests (test-first) |
| `docs/notes/phase-22.md` | notes |

## Testing (test-first; live network under `make test`)

1. `test_inet_checksum` — the internet checksum of a known buffer matches the
   reference value (pure function).
2. `test_arp_resolve` — `arp_resolve(10.0.2.2)` returns the gateway's MAC
   (non-zero) — real ARP exchange.
3. `test_icmp_ping` — `net_ping(10.0.2.2, &ms)` succeeds (an echo reply comes
   back).
4. `test_udp_roundtrip` — bind a UDP socket, `sendto` a DNS query to `10.0.2.3:53`
   (QEMU's DNS), `recvfrom` the response (length > 0) — proves UDP send + receive.
   (If the DNS responder is unavailable, fall back to asserting `sendto` succeeds
   and the socket machinery works.)

The net thread runs during these tests (started by `sched_init`/the test harness),
so real packets flow.

## Success criteria

- Layer tests pass under `make test` (test-first); prior tests stay green; gate
  holds. ARP, ICMP, and UDP work against QEMU's virtual network.
- Live: `make run`, then `ping` in the shell prints a reply from `10.0.2.2`; a TCP
  `connect` to a host service succeeds.

## Out of scope

DHCP (static `10.0.2.15`); IP fragmentation/reassembly; TCP server
(`listen`/`accept`), windowing, congestion control, and Nagle; DNS resolver
(raw queries only); IPv6; `poll`/`select` over sockets. These are clear next
steps.
