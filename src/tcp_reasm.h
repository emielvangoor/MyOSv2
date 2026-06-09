// tcp_reasm.h -- a sequence-number-keyed TCP reassembly queue.
// ===========================================================
//
// TCP runs over IP, which may drop, duplicate, or REORDER packets. A naive
// receiver that only accepts the exactly-next segment must throw away anything
// that arrives early -- so a single lost packet makes it re-request (and the
// sender resend) the whole tail of the stream. The cure is a *reassembly queue*:
// stash bytes that arrive ahead of the in-order position, and hand them to the
// application the moment the gap in front of them is filled.
//
// This is a self-contained data structure with no I/O: you feed it the payload
// of each segment (its starting sequence number + bytes), and read back the
// next contiguous run of in-order bytes. tcp.c owns one of these per connection
// and uses `tcp_reasm_pos()` as its `rcv_nxt` (the next byte it still needs).
//
// Storage is a fixed circular buffer indexed by sequence number: the byte with
// sequence `s` lives at slot `s % TCP_REASM_WIN`, with a parallel bitmap marking
// which slots are filled. Because we only accept sequence numbers inside the
// window [base, base+WIN) and clear slots as we advance past them, a sequence
// number and `seq + WIN` never collide in practice -- provided we never
// advertise a receive window larger than TCP_REASM_WIN.
#pragma once
#include <stdint.h>

#define TCP_REASM_WIN 8192               // capacity in bytes (>= advertised window)

struct tcp_reasm {
    uint32_t base;                       // sequence number mapped to the front
    uint8_t  data[TCP_REASM_WIN];        // byte with sequence s -> data[s % WIN]
    uint8_t  present[TCP_REASM_WIN / 8]; // 1 bit per slot: is data[slot] filled?
};

// (Re)initialise the queue so the next in-order byte expected is `rcv_nxt`.
void tcp_reasm_init(struct tcp_reasm *r, uint32_t rcv_nxt);

// Accept a segment's payload spanning sequence numbers [seq, seq+len). Bytes
// that fall inside the window are stored (idempotently -- a retransmit just
// overwrites with the same data); bytes outside [base, base+WIN) are ignored as
// either already-consumed or too-far-ahead to buffer.
void tcp_reasm_accept(struct tcp_reasm *r, uint32_t seq, const uint8_t *data, int len);

// Pop the next in-order contiguous run -- the bytes with sequence base, base+1,
// ... -- into `out` (up to `max`). Advances `base` past whatever it returns and
// clears those slots. Returns the number of bytes written (0 if the very next
// byte is still missing).
int tcp_reasm_read(struct tcp_reasm *r, uint8_t *out, int max);

// The current in-order position: the sequence number of the next byte still
// missing. tcp.c uses this as rcv_nxt (what it ACKs).
uint32_t tcp_reasm_pos(const struct tcp_reasm *r);
