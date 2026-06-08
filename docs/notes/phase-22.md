# Phase 22 notes — TCP/IP stack: ARP, IPv4, ICMP, UDP, DNS, and `ping`

## What changed

On top of the Phase-21 raw-frame driver we built a small **TCP/IP stack**
(`netstack.c`) and a user-space `ping` that resolves names through **DNS**:

```
$ ping
ping 10.0.2.2: reply in 0 ms
$ ping example.com
ping example.com (104.20.23.154): reply in 0 ms
$ ping https://www.google.com
ping www.google.com (142.251.157.119): reply in 0 ms
```

The layers, bottom-up: **Ethernet** framing → **ARP** (resolve/cache/reply) →
**IPv4** (header + checksum + next-hop routing) → **ICMP** echo and **UDP** →
**DNS** resolver. A `resolve()` and a `ping()` syscall expose it to userland.

## The pump model (no net thread)

There is no background networking thread. Each blocking call — `arp_resolve`,
`net_ping`, `net_resolve` — drives the receive path itself by spinning
`net_pump()`, which pulls one frame off the NIC and dispatches it up the stack.
This suits our cooperative single-core kernel and, crucially, works inside the
test harness (where the timer/scheduler isn't running). The cost is a busy-wait
while a request is outstanding; see "Limits".

## IPv4 + ICMP

`ip_send` picks the **next hop** — the gateway unless the destination is on our
`/24` — ARP-resolves it, fills in the 20-byte header (version/IHL, total length,
TTL 64, protocol, the one's-complement header checksum) and ships it as an
`0x0800` frame. `ip_input` validates it's addressed to us and demuxes by protocol
(1 = ICMP, 17 = UDP). ICMP answers echo **requests** (so we're pingable) and
matches echo **replies** to the outstanding `ping` by id.

## UDP + DNS

UDP is just enough to carry DNS: an 8-byte header with the checksum left **0**
("not computed", which IPv4 explicitly permits). DNS is two pure, unit-tested
functions plus a thin live wrapper:

- `dns_build_query` — encodes the hostname as length-prefixed **QNAME labels**
  (`a.bc` → `[1]a[2]bc[0]`) and asks for an A record with recursion desired.
- `dns_parse_answer` — walks the answer section, **following compression
  pointers** (`0xC0xx`) and **skipping CNAMEs** by their RDLENGTH, and returns
  the first A record. RCODE ≠ 0 (e.g. NXDOMAIN) → failure.
- `net_resolve` — sends the query to QEMU's resolver at `10.0.2.3:53`, pumps for
  the reply on our ephemeral source port, and parses it.

QEMU's SLIRP forwards both ICMP (to the real host's ping path) and DNS (to the
host's resolver), so `ping`/`resolve` reach the actual internet.

## argv reaches userland

`ping <host>` needed real arguments, so **exec now passes `argv`**.
`proc_setup_argv` lays the strings and a NULL-terminated pointer array on the new
program's stack (within the top page, written through the identity map so the
target address space needn't be active yet), and the program enters with
`x0 = argc`, `x1 = argv`. The shell tokenizes the command line into `argv`.

### A bug worth remembering

argc kept arriving as 0. `proc_exec` set `tf->x[0] = argc`, but `do_syscall`
overwrites `tf->x[0]` with the syscall's **return value** *after* the handler
runs — clobbering it. Fix: leave `x0` to the return path (`return argc`) and put
`argv` in `x1` (which isn't clobbered). The on-stack-layout unit test passed the
whole time because it never went through the syscall return.

## Testing

8 new tests. The DNS encode/decode pair is fully **offline and deterministic**
(query layout; A-record extraction with a compression pointer; CNAME-then-A;
RCODE error). The exec-argv test inspects the bytes `proc_setup_argv` writes
(pointer array, NULL terminator, strings). Live tests (needing SLIRP) resolve
`example.com` and ping the gateway. 92 tests total, green under `make test`.

## Limits

- **Busy-wait** while a request is outstanding (no IRQ-driven wakeup); a long
  request also means the shell isn't reading stdin, so pasting many lines at once
  can overflow QEMU's 16-byte UART FIFO (interactive typing is fine).
- No TCP yet (only ICMP/UDP); no IP fragmentation/reassembly; UDP TX checksum
  omitted; single outstanding DNS request; tiny ARP cache; no DHCP (addresses are
  the fixed SLIRP ones). A real socket API and TCP are future work.
