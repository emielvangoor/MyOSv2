# Phase 23 — TCP hardening + networking completeness

Phase 22 gave us a TCP *client on a reliable path*. Phase 23 turns that into
something closer to a real stack, one milestone at a time. Each milestone is its
own test-first cycle gated on `make test`.

---

## 23.1 — Out-of-order reassembly

**The gap it closed.** The Phase-22 receiver only accepted the segment whose
sequence number was *exactly* `rcv_nxt`. Anything that arrived early was dropped
and re-ACKed, so a single lost or reordered packet forced the sender to resend
the entire tail behind it. IP is free to drop, duplicate, and reorder, so this
is a real correctness gap the moment the path is less than perfect. (QEMU's
SLIRP delivers in order, which is why the client worked anyway.)

**The design — a reassembly queue (`src/tcp_reasm.{h,c}`).** A small, pure data
structure with no I/O, so it is trivially unit-testable:

- Storage is a fixed circular byte buffer (`TCP_REASM_WIN` = 8 KiB) plus a
  presence bitmap, both indexed by `sequence % WIN`.
- `tcp_reasm_accept(seq, data, len)` stores every byte whose sequence falls in
  the window `[base, base+WIN)` and ignores the rest (already consumed, or too
  far ahead to buffer). Retransmits and overlaps are idempotent.
- `tcp_reasm_read(out, max)` pops the next *contiguous* run starting at `base`,
  advancing past it. It returns 0 while the very next byte is still missing.
- `tcp_reasm_pos()` is the new `rcv_nxt` (the next byte we still need / ACK).

Sequence numbers are compared with signed differences, so everything stays
correct across the 32-bit wrap (there is a dedicated test for that).

**Wiring into `tcp_input`.** The old "if `seq == rcv_nxt` then push else re-ACK"
branch became: `accept()` the payload, drain everything now contiguous into the
app-visible ring, set `rcv_nxt = pos()`. An out-of-order segment buffers
silently and produces a *duplicate ACK* — which is exactly the signal a sender's
fast-retransmit logic will key on in 23.6.

**FIN ordering.** A FIN occupies the sequence number just past its data. We
record it (`have_fin`, `fin_seq`) but only consume it — advance `rcv_nxt`, signal
EOF — once `rcv_nxt` has actually reached it. Otherwise an out-of-order FIN would
report end-of-stream ahead of a still-missing segment.

**Tests.** `tcp: reasm in-order run`, `… out-of-order fill`, `… wraps seq space`,
`… dup + out-of-window`. End-to-end: `/bin/http` still fetches example.com
(`HTTP/1.1 200 OK`, 797 bytes) over the new receive path.

**Why the integration isn't unit-tested directly.** Driving `tcp_input` needs a
live ESTABLISHED connection (ports, ISNs, a peer). Rather than pollute the
production API with a test seam, the *algorithm* — the part with real edge cases
— lives in `tcp_reasm` and is exhaustively unit-tested; the `tcp_input` wiring is
a thin, reviewed adapter verified end-to-end by the `http` demo.

---

## 23.2 — Real retransmission (RTO estimation + Karn + backoff)

**The gap it closed.** Phase 22 resent the in-flight segment on a *fixed* 500 ms
timer and pinned the caller's buffer for the duration. A fixed timer is wrong on
both ends: too eager on a slow path (spurious resends), too patient on a fast one
(a real loss stalls for half a second). And there was no round-trip learning at
all.

**The design — an RTO estimator (`src/tcp_rto.{h,c}`).** Another pure module:
feed it round-trip-time measurements, read back a retransmission timeout.

- RFC 6298 smoothing, in microseconds: first sample seeds `SRTT = R`,
  `RTTVAR = R/2`; later samples use `RTTVAR = 3/4 RTTVAR + 1/4 |SRTT−R|` then
  `SRTT = 7/8 SRTT + 1/8 R` (the standard shift forms). `RTO = SRTT + 4·RTTVAR`.
- Clamped to `[200 ms, 60 s]`. RFC mandates a 1 s floor; we relax it to 200 ms
  (a common real-world value, e.g. Linux) so the client stays responsive on
  QEMU's sub-millisecond LAN. Initial RTO before any sample is 1 s.
- `tcp_rto_backoff()` doubles the RTO on each timeout (capped, overflow-guarded).

**Karn's algorithm** lives in the caller and has two halves: (1) never sample an
RTT from a segment that was retransmitted — the ACK is ambiguous; (2) on timeout,
back off instead. A clean later sample collapses the backed-off RTO back to the
measured estimate.

**Wiring into `tcp.c`.** Each connection now owns a `struct tcp_rto`. The single
outstanding segment is copied into the connection (`sndbuf`/`sndlen`/`sndseq`),
so the caller's buffer is no longer pinned. `tcp_send` and `tcp_connect` resend
after `tcp_rto_get()` rather than a constant, sample the RTT on a clean ACK (and
seed from the SYN/SYN-ACK), set `snd_retx` to suppress sampling once resent, and
`tcp_rto_backoff()` on each timeout.

**Scope note.** With the current model there is at most one segment in flight, so
the "retransmit queue" is that one segment. True pipelined multi-segment
retransmission arrives alongside the send-window rework in congestion control
(23.6).

**Tests.** `tcp: rto first sample (RFC6298)`, `tcp: rto backoff + clamp` (doubling,
the 60 s cap with no overflow, the floor, and backoff collapse). End-to-end:
`/bin/http` still fetches example.com (200 OK, 797 bytes).

---

## 23.3 — Flow control (advertised windows)

**The gap it closed.** Phase 22 advertised a fixed 8 KiB window regardless of how
full the receive buffer actually was, and ignored the peer's window entirely
(safe only because it never had more than one segment in flight). A real receiver
must slow the sender down when its buffer fills, and a real sender must never
overrun the receiver's.

**The window arithmetic (pure, exposed for tests).**
- `tcp_advertise_wnd(free_bytes)` — the window to advertise: free receive-buffer
  space, capped to what we can buffer ahead (`TCP_REASM_WIN`) and the 16-bit field.
- `tcp_window_avail(snd_una, snd_nxt, peer_wnd, mss)` — how many new bytes we may
  send: room left in the peer's window beyond what's in flight, capped at the MSS.

**Receive side.** Every outgoing segment now carries `tcp_advertise_wnd(ring_free)`
instead of a constant, and the connection remembers it in `last_adv`. When the app
drains the ring (`tcp_recv`), if our advertisable window has grown by at least an
MSS over `last_adv`, we send a bare ACK as a **window update** — silly-window
avoidance, so a stale small window doesn't keep the peer throttled.

**Send side.** The connection tracks the peer's `snd_wnd` from every segment's
window field (seeded at the SYN-ACK). `tcp_send` sends only `tcp_window_avail(...)`
bytes; while the window is closed it waits, probing periodically with a bare ACK
(a light zero-window persist) until it reopens.

**Scope note.** Single segment in flight still, so honoring the window rarely
bites in the demo (servers offer tens of KiB); the machinery is what matters, and
it composes with the pipelining added in 23.6/23.8. Window *scaling* (RFC 7323)
is still out.

**Tests.** `tcp: flow-control windows` (advertise cap/floor; sendable vs in-flight
and MSS). End-to-end: `/bin/http` still fetches example.com (200 OK, 797 bytes).

---

## 23.4 — TCP server: listen/accept + a tiny HTTP server

**The gap it closed.** Everything so far was a *client*: `tcp_connect` actively
drives the handshake. A server does the opposite — it sits in LISTEN and reacts to
an inbound SYN (passive open). The stack had no LISTEN/SYN_RCVD states and no way
to demultiplex a SYN to a listener.

**The state machine.** Two new states, `LISTEN` and `SYN_RCVD`. `tcp_input`'s
demux now has three outcomes: (1) an exact 4-tuple match → the existing per-conn
handling; (2) no match but a bare SYN to a `LISTEN` port → spawn a child
connection from the pool, reply SYN-ACK, enter `SYN_RCVD`; (3) otherwise drop. A
child shares the listener's local port but carries a concrete peer, so its
subsequent segments (the final ACK, then data) match exactly. The final ACK
(`ack == snd_nxt`) promotes the child to `ESTABLISHED`; a duplicate SYN (our
SYN-ACK was lost) re-sends the SYN-ACK.

**API.** `tcp_listen(c, port)` makes a connection a wildcard listener;
`tcp_accept(listener)` blocks (pumping the network) until a child reaches
`ESTABLISHED` and hands it out (`accepted` flag prevents double-hand-out). The
socket layer wraps these as `socket_listen` / `socket_accept` (the latter wraps
the already-established child in a fresh socket *without* allocating another
`tcp_conn`), exposed as `SYS_LISTEN` / `SYS_ACCEPT` and the `listen()`/`accept()`
ulib calls. The connection pool grew from 4 to 8 to hold a listener plus children.

**Demo.** `/bin/httpd` — `socket → bind → listen → accept`, read the request,
write a canned `200 OK`, close, repeat. The Makefile adds `hostfwd=tcp::8080-:8080`
so the guest server is reachable from the host.

**Verification.** This is the one phase whose new behavior can't be cleanly
unit-tested in-kernel: driving the passive handshake needs the server's
timer-derived ISN to forge the final ACK, which the test can't know without a
test-only seam into the connection internals. Instead it's verified by the
strongest possible test — a **real external TCP client**: boot, run `httpd`, then
from the host `curl http://localhost:8080/` returns `Hello from MyOSv2!`, twice
(guest logs `served request #1`, `#2`). The 109 unit tests remain the regression
net for reassembly/RTO/flow-control.

To reproduce: `make run`, type `httpd`, then in another terminal
`curl http://localhost:8080/`.

---

## 23.5 — Socket API polish: poll() + shutdown()

**The gap it closed.** Every I/O call blocked on exactly one fd. A program that
must watch several at once (a server with many connections, or anything mixing a
socket and a pipe) had no way to ask "which of these is ready?" without
committing to a blocking read on one.

**poll() (`src/poll.{h,c}`).** The pure heart is `poll_scan`: one non-blocking
pass that fills each `pollfd`'s `revents` from the fd's current readiness
(POLLIN/POLLOUT/POLLHUP, POLLERR for a bad fd). Readiness dispatches by file
type to small predicates: `pipe_readable/writable/hangup`, `socket_readable/
writable` → `tcp_readable/writable`. `SYS_POLL` wraps `poll_scan` in the standard
pump-and-sleep loop until at least one fd is ready or the timeout (ms) elapses;
`timeout == 0` makes it a non-blocking probe, a signal makes it return -1 (EINTR).

**shutdown() (TCP half-close).** `tcp_shutdown` sends a FIN and moves to FIN_WAIT
but, unlike `tcp_close`, keeps the connection so the app can still read what the
peer sends before its own FIN. Exposed as `socket_shutdown(fd, SHUT_WR)` /
`SYS_SOCKSHUT` / `sock_shutdown()`.

**Tests.** `poll: pipe readiness scan` builds two pipe ends in-kernel and checks
the scan: empty pipe → write end writable, read end not readable; after a write →
both ready; a closed fd → POLLERR. End-to-end: `/bin/polldemo` forks a child that
writes a pipe after 300 ms while the parent `poll()`s it with a 2 s timeout —
prints `ready -> read "ping"`.

**A harness quirk found along the way.** The poll test first hung when placed
after the live-network tests: those call `net_wait` on the single boot thread,
and `sleep_ticks` leaves it `SLEEPING` when `schedule()` finds nothing else
runnable (harmless in the real OS, where the idle thread is always runnable and
the timer wakes sleepers, but it lingers in the bare test harness). Moving the
poll test next to the pipe tests — a cleaner grouping anyway — runs it in a clean
environment. Also: `hostfwd` moved off the shared QEMU flags onto `make run`
only, so `make test` never tries to bind host port 8080.

**Deferred (milestone-picking).** `select` (redundant with `poll`),
non-blocking-fd flag (`fcntl`/`O_NONBLOCK`), `getsockname`/`getpeername`,
`setsockopt`, and `recv`/`send` flags — all straightforward additions on this
foundation, left for later.

---

## 23.6 — Congestion control (Reno)

**The gap it closed.** Flow control (23.3) stops a sender overrunning the
*receiver*. Nothing stopped it overrunning the *network* — the routers/links in
between, which advertise no window. Reno infers the path's capacity from ACKs and
losses and keeps a second limit on in-flight data, the congestion window `cwnd`.

**The control law (`src/tcp_cc.{h,c}`).** A pure module, fully unit-tested:
- **Slow start** (cwnd < ssthresh): +1 MSS per ACK — exponential per round-trip.
- **Congestion avoidance** (cwnd ≥ ssthresh): +mss²/cwnd per ACK — ~+1 MSS/RTT.
- **Triple duplicate ACK** → fast retransmit: ssthresh = max(cwnd/2, 2·MSS),
  cwnd = ssthresh + 3·MSS (fast recovery); each further dupack inflates cwnd; the
  next new ACK deflates back to ssthresh and exits recovery.
- **Timeout** (severe): ssthresh = max(cwnd/2, 2·MSS), cwnd = 1 MSS — slow-start
  again.

**Wiring into `tcp.c`.** Each connection owns a `tcp_cc`. A new cumulative ACK
calls `tcp_cc_on_ack` (grow); a pure duplicate ACK (same ack+window, no data,
data still in flight) calls `tcp_cc_on_dupack` and fast-retransmits the
outstanding segment on the third; a data-segment timeout calls
`tcp_cc_on_timeout`. The send is now bounded by **both** windows:
`n = min(peer_window_room, cwnd − inflight, len)`.

**Tests.** `tcp: cc slow start`, `tcp: cc avoidance + loss` (the CA increment, the
3-dupack fast-retransmit signal with exact ssthresh/cwnd, recovery deflate, and
the timeout collapse). End-to-end: `/bin/http` still fetches example.com (200 OK).

**Scope note (honest).** With the current one-segment-in-flight send path, cwnd
rarely *binds* (we never have more than an MSS outstanding) and duplicate ACKs
don't naturally arise, so fast-retransmit is dormant — but slow-start/avoidance
*are* exercised by every real transfer, and the whole control law is verified by
unit tests. cwnd becomes load-bearing once sends pipeline (23.8).

---

## 23.7 — Full state machine + teardown (CLOSE_WAIT, TIME_WAIT, RST)

**The gap it closed.** Close was a single ad-hoc `FIN_WAIT` state that just
flipped to CLOSED, and a segment hitting no connection was silently dropped (so a
confused peer would retransmit forever). Neither is how TCP actually closes.

**The states (RFC 793).** The lone `FIN_WAIT` became the real set: active close
`FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT` (and `CLOSING` for a simultaneous close);
passive close `CLOSE_WAIT → LAST_ACK`. A `synchronized(state)` predicate gates
the data/ACK/FIN processing across all of them. `tcp_input` now drives:
- **Our FIN acked** (`snd_una == snd_nxt` once the FIN bumped `snd_nxt`):
  FIN_WAIT_1→FIN_WAIT_2, CLOSING→TIME_WAIT, LAST_ACK→CLOSED.
- **Peer's FIN arrives** (consumed in order): ESTABLISHED→CLOSE_WAIT,
  FIN_WAIT_2→TIME_WAIT, FIN_WAIT_1→TIME_WAIT (if it also acked our FIN) or CLOSING.
- **TIME_WAIT** lingers `2·MSL` (MSL shortened to 250 ms for a fast LAN), re-ACKing
  a retransmitted FIN and restarting the timer, before the slot is freed.

`tcp_close` sends the FIN appropriate to the state (ESTABLISHED → active,
CLOSE_WAIT → passive/LAST_ACK) and pumps the exchange to completion;
`tcp_shutdown` (half-close) just moves to FIN_WAIT_1 and lets `tcp_input` finish.

**RST generation.** A segment matching no connection (and not a fresh SYN to a
listener, and not itself a RST) now gets a RST reply. `tcp_rst_fields` computes
its seq/ack per RFC 793 — seq from the offending ACK, or ack = seq+len with the
ACK flag if the segment had no ACK — and is unit-tested.

**Tests.** `tcp: RST reply fields` (both RST cases). End-to-end: `/bin/http`
closes actively (FIN_WAIT_1→…→TIME_WAIT) and still fetches example.com cleanly;
the host curls `/bin/httpd` three times in a row — each is a passive close
(CLOSE_WAIT→LAST_ACK→CLOSED) and the connection slot is reused, so nothing leaks.

**Scope note.** TIME_WAIT lingers only inside the (bounded) `tcp_close` pump
rather than via a background timer, so the "port quarantine" lasts the close
call, not a true 2·MSL wall-clock window — adequate here since each `tcp_connect`
picks a fresh ephemeral port anyway.

---

## 23.8 — Larger transfers: segmentation + Nagle

**The gap it closed.** `tcp_send` sent exactly one ≤MSS segment and made the
caller loop for the rest — so a write bigger than 1400 bytes was the caller's
problem, only one segment was ever in flight, and the congestion window from 23.6
never actually bound anything.

**The send queue.** Each connection now holds a `TCP_SNDBUF` (16 KiB) chunk of the
caller's data with `snd_base` as the sequence of `sndbuf[0]`. `tcp_send` copies
the write a chunk at a time and calls `tcp_flush_chunk`, which:
- `tcp_send_window` pushes as many new MSS segments as the **effective window** —
  `min(peer window, cwnd)` — allows, pipelining several in flight at once;
- waits for cumulative ACKs to slide `snd_una` forward (freeing window for more);
- on RTO, `tcp_retx_oldest` replays from `snd_una` (a real retransmit queue, not
  just "resend the last segment"), backs off, and collapses cwnd;
- samples RTT once per flight, Karn-style.

This is also what makes 23.6 live: with several segments outstanding, the receiver
*can* now emit the duplicate ACKs that drive fast retransmit, and cwnd genuinely
limits how much goes out.

**Nagle / silly-window.** The per-segment decision is the pure, tested
`tcp_next_seg(unsent, inflight, win, mss)`: send `min(unsent, room, mss)`, but
return 0 (wait) for a sub-MSS segment while data is still in flight — coalescing
small writes and avoiding silly-window dribbles. When nothing is in flight it
always sends, so a lone small write goes immediately.

**Tests.** `tcp: next segment (Nagle/window)` covers full-MSS, lone-small,
window-limited, Nagle-hold, window-full, and empty cases. End-to-end: `/bin/httpd`
now serves a **4000-byte** body; a host `curl … | wc -c` gets exactly 4000 bytes,
and the md5 is identical across repeated fetches — the multi-segment transfer
reassembles byte-for-byte. `/bin/http` still fetches example.com.

**Deferred.** Delayed ACKs — without a background ACK timer they risk the classic
Nagle/delayed-ACK deadlock, and our ACK volume is already modest; left out
deliberately. (Real peers' delayed-ACK timers already cover the inbound side.)
