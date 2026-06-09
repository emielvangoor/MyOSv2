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
