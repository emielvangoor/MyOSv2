// tcp_reasm.c -- sequence-number-keyed TCP reassembly queue. See tcp_reasm.h.
//
// The store is a circular byte buffer plus a presence bitmap, both indexed by
// (sequence number mod TCP_REASM_WIN). `base` is the sequence number of the next
// in-order byte; everything we hand out comes from there forward. Sequence
// numbers are compared with signed differences so the comparisons stay correct
// across the 32-bit wrap.
#include "tcp_reasm.h"

// Is slot `i` (a physical buffer index) currently filled?
static int slot_present(const struct tcp_reasm *r, unsigned i)
{
    return (r->present[i >> 3] >> (i & 7)) & 1;
}
static void slot_set(struct tcp_reasm *r, unsigned i)
{
    r->present[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static void slot_clear(struct tcp_reasm *r, unsigned i)
{
    r->present[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

void tcp_reasm_init(struct tcp_reasm *r, uint32_t rcv_nxt)
{
    r->base = rcv_nxt;
    for (unsigned i = 0; i < sizeof(r->present); i++) { r->present[i] = 0; }
}

void tcp_reasm_accept(struct tcp_reasm *r, uint32_t seq, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        uint32_t s = seq + (uint32_t)i;
        // Offset from the in-order front, as a signed distance so wrap is safe.
        int32_t off = (int32_t)(s - r->base);
        if (off < 0 || off >= TCP_REASM_WIN) { continue; }  // old or too far ahead
        unsigned slot = (unsigned)(s % TCP_REASM_WIN);
        r->data[slot] = data[i];
        slot_set(r, slot);
    }
}

int tcp_reasm_read(struct tcp_reasm *r, uint8_t *out, int max)
{
    int n = 0;
    while (n < max) {
        unsigned slot = (unsigned)(r->base % TCP_REASM_WIN);
        if (!slot_present(r, slot)) { break; }     // next in-order byte missing
        out[n++] = r->data[slot];
        slot_clear(r, slot);
        r->base++;
    }
    return n;
}

uint32_t tcp_reasm_pos(const struct tcp_reasm *r)
{
    return r->base;
}
