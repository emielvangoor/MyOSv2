// tcp.c -- a minimal TCP client.
// ==============================
//
// Enough TCP to fetch a page over QEMU user-net: the 3-way handshake, cumulative
// ACKs, a simple retransmit of unacked segments, and a FIN close. Segments that
// arrive out of order are buffered in a per-connection reassembly queue
// (tcp_reasm.h) and released in order once the gap ahead of them is filled, so a
// single dropped/reordered packet no longer forces the whole tail to be resent.
//
// Sequence numbers are 32-bit and wrap; compare them with signed differences
// (seq_gt/seq_ge) so the arithmetic stays correct across the wrap.

#include <stdint.h>
#include "tcp.h"
#include "tcp_reasm.h"
#include "tcp_rto.h"
#include "net.h"
#include "sched.h"
#include "timer.h"

// TCP flag bits (in the 13th header byte).
#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT };

#define NCONN     4
#define TCP_RXBUF 16384
#define TCP_MSS   1400          // largest payload we put in one segment

struct tcp_conn {
    int      used;
    int      state;
    uint16_t lport, rport;
    uint32_t rip;
    uint32_t snd_nxt;           // next sequence number to send
    uint32_t snd_una;           // oldest unacknowledged sequence number
    uint32_t rcv_nxt;           // next sequence number expected from the peer
    uint16_t snd_wnd;           // peer's advertised receive window (flow control)
    uint16_t last_adv;          // the receive window we last advertised to the peer
    uint8_t  rx[TCP_RXBUF];     // received-data ring (in-order bytes, app-visible)
    int      rxhead, rxtail;
    struct tcp_reasm reasm;     // holds out-of-order segments until the gap fills
    int      have_fin;          // peer's FIN has been seen (maybe out of order)...
    uint32_t fin_seq;           // ...at this sequence number (consumed when in order)
    int      peer_fin;          // FIN now in order -> EOF once the ring drains
    int      reset;             // peer sent RST

    // Retransmission (Phase 23.2). The single outstanding segment is kept here --
    // a copy, so the caller's buffer isn't pinned during the send -- and resent
    // after an adaptive RTO with exponential backoff.
    struct tcp_rto rto;         // RFC 6298 round-trip-time / timeout estimator
    uint8_t  sndbuf[TCP_MSS];   // bytes of the in-flight segment (for retransmit)
    int      sndlen;            // its length
    uint32_t sndseq;            // its starting sequence number
    uint64_t snd_time;          // when it was first transmitted (for RTT sampling)
    int      snd_retx;          // was it retransmitted? (Karn: then don't sample RTT)
};

static struct tcp_conn conns[NCONN];
static uint16_t next_lport = 40000;

// ---- byte helpers ----
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

// ---- receive ring ----
static int  ring_empty(struct tcp_conn *c) { return c->rxhead == c->rxtail; }
static void ring_push(struct tcp_conn *c, uint8_t b)
{
    int next = (c->rxhead + 1) % TCP_RXBUF;
    if (next == c->rxtail) { return; }     // full -> drop (window bounds this)
    c->rx[c->rxhead] = b; c->rxhead = next;
}
static int ring_pop(struct tcp_conn *c, uint8_t *out, int max)
{
    int n = 0;
    while (n < max && c->rxtail != c->rxhead) {
        out[n++] = c->rx[c->rxtail];
        c->rxtail = (c->rxtail + 1) % TCP_RXBUF;
    }
    return n;
}

// ---- checksum (pseudo-header + segment) ----
uint16_t tcp_checksum(uint32_t sip, uint32_t dip, const uint8_t *seg, int len)
{
    uint32_t sum = 0;
    sum += (sip >> 16) & 0xffff; sum += sip & 0xffff;
    sum += (dip >> 16) & 0xffff; sum += dip & 0xffff;
    sum += 6;                              // protocol
    sum += (uint32_t)len;                  // TCP length
    for (int i = 0; i + 1 < len; i += 2) { sum += (uint32_t)((seg[i] << 8) | seg[i + 1]); }
    if (len & 1) { sum += (uint32_t)(seg[len - 1] << 8); }
    while (sum >> 16) { sum = (sum & 0xffff) + (sum >> 16); }
    return (uint16_t)(~sum);
}

// ---- flow control: advertised-window arithmetic (Phase 23.3) ----
uint16_t tcp_advertise_wnd(int free_bytes)
{
    if (free_bytes < 0) { free_bytes = 0; }
    // Never promise more than we can buffer ahead of rcv_nxt, nor more than the
    // 16-bit window field can hold.
    if (free_bytes > TCP_REASM_WIN) { free_bytes = TCP_REASM_WIN; }
    if (free_bytes > 65535) { free_bytes = 65535; }
    return (uint16_t)free_bytes;
}

int tcp_window_avail(uint32_t snd_una, uint32_t snd_nxt, uint16_t peer_wnd, int mss)
{
    int32_t inflight = (int32_t)(snd_nxt - snd_una);    // bytes sent, not yet acked
    int32_t usable   = (int32_t)peer_wnd - inflight;    // room left in the window
    if (usable < 0) { usable = 0; }
    if (usable > mss) { usable = mss; }
    return usable;
}

// Bytes currently free in the receive ring (one slot is kept empty to tell full
// from empty), which is what we advertise to the peer.
static int ring_free(struct tcp_conn *c)
{
    int used = (c->rxhead - c->rxtail + TCP_RXBUF) % TCP_RXBUF;
    return TCP_RXBUF - 1 - used;
}

// ---- transmit one segment (header + optional data) ----
static void tcp_xmit_seq(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                         const uint8_t *data, int dlen)
{
    static uint8_t seg[20 + TCP_MSS];
    put16(seg + 0, c->lport);
    put16(seg + 2, c->rport);
    put32(seg + 4, seq);
    put32(seg + 8, c->rcv_nxt);
    seg[12] = 5 << 4;                      // data offset = 5 words (20 bytes)
    seg[13] = flags;
    // Advertise our actual free receive-buffer space (flow control): as the app
    // falls behind and the ring fills, this shrinks and throttles the peer; as it
    // drains, it grows again. Remember what we advertised so tcp_recv knows when a
    // window update is worth sending.
    uint16_t adv = tcp_advertise_wnd(ring_free(c));
    c->last_adv = adv;
    put16(seg + 14, adv);
    put16(seg + 16, 0);                    // checksum (filled below)
    put16(seg + 18, 0);                    // urgent pointer
    for (int i = 0; i < dlen; i++) { seg[20 + i] = data[i]; }
    int len = 20 + dlen;
    put16(seg + 16, tcp_checksum(IP_OURS, c->rip, seg, len));
    net_ip_send(c->rip, 6, seg, len);
}
static void tcp_xmit(struct tcp_conn *c, uint8_t flags, const uint8_t *data, int dlen)
{
    tcp_xmit_seq(c, flags, c->snd_nxt, data, dlen);
}

// A blocking call returns early if a signal is posted (EINTR).
static int tcp_interrupted(void)
{
    struct thread *t = sched_current();
    return t && t->sig_pending;
}

// ---- the receive state machine ----
void tcp_input(uint32_t src_ip, const uint8_t *seg, int len)
{
    if (len < 20) { return; }
    uint16_t sport = get16(seg + 0);
    uint16_t dport = get16(seg + 2);
    uint32_t seq   = get32(seg + 4);
    uint32_t ack   = get32(seg + 8);
    int doff = (seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint16_t wnd  = get16(seg + 14);       // peer's advertised receive window
    if (doff < 20 || doff > len) { return; }
    const uint8_t *data = seg + doff;
    int dlen = len - doff;

    struct tcp_conn *c = 0;
    for (int i = 0; i < NCONN; i++) {
        if (conns[i].used && conns[i].lport == dport &&
            conns[i].rip == src_ip && conns[i].rport == sport) { c = &conns[i]; break; }
    }
    if (!c) { return; }

    if (flags & RST) { c->reset = 1; c->state = CLOSED; return; }

    if (c->state == SYN_SENT) {
        if ((flags & SYN) && (flags & ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;          // peer's ISN + 1
            c->snd_una = ack;
            c->snd_wnd = wnd;              // peer's initial receive window
            c->state = ESTABLISHED;
            tcp_reasm_init(&c->reasm, c->rcv_nxt);   // start reassembly from here
            tcp_xmit(c, ACK, 0, 0);        // finish the handshake
        }
        return;
    }

    if (c->state == ESTABLISHED || c->state == FIN_WAIT) {
        if ((flags & ACK) && seq_gt(ack, c->snd_una)) { c->snd_una = ack; }
        c->snd_wnd = wnd;                  // track the peer's latest window update

        // Hand the payload to the reassembly queue (it ignores anything outside
        // the window), then drain whatever is now contiguous into the app-visible
        // ring. An out-of-order segment buffers silently and yields nothing here;
        // the missing segment, when it arrives, releases the whole run at once.
        if (dlen > 0) { tcp_reasm_accept(&c->reasm, seq, data, dlen); }
        uint8_t tmp[TCP_MSS];
        int got;
        while ((got = tcp_reasm_read(&c->reasm, tmp, sizeof(tmp))) > 0) {
            for (int i = 0; i < got; i++) { ring_push(c, tmp[i]); }
        }
        c->rcv_nxt = tcp_reasm_pos(&c->reasm);

        // A FIN occupies the sequence number just past its data. Record it, but
        // only consume it (advance rcv_nxt, signal EOF) once every preceding byte
        // has arrived -- otherwise an out-of-order FIN would falsely report EOF
        // ahead of a still-missing segment.
        if (flags & FIN) { c->have_fin = 1; c->fin_seq = seq + (uint32_t)dlen; }
        if (c->have_fin && !c->peer_fin && c->rcv_nxt == c->fin_seq) {
            c->rcv_nxt += 1;
            c->peer_fin = 1;
        }

        // Cumulatively ACK anything that advanced (or could advance) our window;
        // a pure duplicate/out-of-order segment still gets a duplicate ACK, which
        // is exactly what a sender's fast-retransmit logic later keys on.
        if (dlen > 0 || (flags & FIN)) { tcp_xmit(c, ACK, 0, 0); }
        if (c->peer_fin && c->state == FIN_WAIT) { c->state = CLOSED; }
    }
}

// ---- client API ----
struct tcp_conn *tcp_new(void)
{
    for (int i = 0; i < NCONN; i++) {
        if (!conns[i].used) {
            struct tcp_conn *c = &conns[i];
            c->used = 1; c->state = CLOSED;
            c->rxhead = c->rxtail = 0; c->peer_fin = 0; c->reset = 0;
            c->have_fin = 0; c->fin_seq = 0;
            c->sndlen = 0; c->snd_retx = 0;
            c->snd_wnd = 0; c->last_adv = 0;
            tcp_rto_init(&c->rto);
            return c;
        }
    }
    return 0;
}

int tcp_connect(struct tcp_conn *c, uint32_t ip, uint16_t port)
{
    if (!c) { return -1; }
    c->rip = ip; c->rport = port;
    c->lport = next_lport++;
    if (next_lport == 0) { next_lport = 40000; }
    c->snd_una = c->snd_nxt = (uint32_t)timer_now_us();   // initial sequence number
    c->state = SYN_SENT;

    tcp_xmit(c, SYN, 0, 0);
    uint32_t isn = c->snd_nxt;
    c->snd_nxt = isn + 1;                  // SYN consumes one sequence number

    uint64_t start = timer_now_us(), last_tx = start;
    int retx = 0;                          // Karn: was the SYN ever resent?
    while (timer_now_us() - start < 5000000) {     // 5 s
        net_pump();
        if (c->state == ESTABLISHED) {
            // Seed the RTT estimator from the handshake, but only if the SYN was
            // not retransmitted (otherwise the sample is ambiguous -- Karn).
            if (!retx) { tcp_rto_sample(&c->rto, (int32_t)(timer_now_us() - start)); }
            return 0;
        }
        if (c->reset) { return -1; }
        if (tcp_interrupted()) { return -1; }
        if (timer_now_us() - last_tx > (uint64_t)tcp_rto_get(&c->rto)) {
            tcp_xmit_seq(c, SYN, isn, 0, 0);    // retransmit the SYN
            tcp_rto_backoff(&c->rto);           // exponential backoff
            retx = 1;
            last_tx = timer_now_us();
        }
        net_wait(20);
    }
    return -1;                             // handshake timed out
}

int tcp_send(struct tcp_conn *c, const void *buf, int len)
{
    if (!c || c->state != ESTABLISHED) { return -1; }
    if (len > TCP_MSS) { len = TCP_MSS; }

    // Flow control: never send past the peer's advertised window. While it is
    // closed (zero window), wait, probing periodically with a bare ACK so we
    // still hear the window-update even if one was lost (a light persist timer).
    uint64_t pstart = timer_now_us(), plast = pstart;
    while (tcp_window_avail(c->snd_una, c->snd_nxt, c->snd_wnd, len) == 0) {
        if (c->reset) { return -1; }
        if (tcp_interrupted()) { return -1; }
        if (timer_now_us() - pstart > 5000000) { return 0; }   // best effort: give up
        net_pump();
        if (timer_now_us() - plast > (uint64_t)tcp_rto_get(&c->rto)) {
            tcp_xmit(c, ACK, 0, 0);        // window probe (also re-advertises ours)
            plast = timer_now_us();
        }
        net_wait(20);
    }
    // Send only as much as the window allows right now (<= len <= MSS). The caller
    // gets the count and resends the remainder; larger writes are split in 23.8.
    int n = tcp_window_avail(c->snd_una, c->snd_nxt, c->snd_wnd, len);

    // Copy into the connection's retransmit buffer so we can resend without the
    // caller keeping `buf` alive, then transmit the segment once.
    const uint8_t *src = buf;
    for (int i = 0; i < n; i++) { c->sndbuf[i] = src[i]; }
    c->sndlen = n;
    c->sndseq = c->snd_nxt;
    c->snd_retx = 0;

    tcp_xmit_seq(c, PSH | ACK, c->sndseq, c->sndbuf, n);
    c->snd_nxt = c->sndseq + (uint32_t)n;
    c->snd_time = timer_now_us();

    uint64_t start = c->snd_time, last_tx = start;
    while (timer_now_us() - start < 5000000) {
        net_pump();
        if (seq_ge(c->snd_una, c->snd_nxt)) {                 // fully acked
            // Karn's algorithm: only feed the estimator a round-trip time when
            // the acked segment was never retransmitted (otherwise we can't tell
            // which transmission the ACK answered).
            if (!c->snd_retx) {
                tcp_rto_sample(&c->rto, (int32_t)(timer_now_us() - c->snd_time));
            }
            return n;
        }
        if (c->reset) { return -1; }
        if (tcp_interrupted()) { return -1; }
        if (timer_now_us() - last_tx > (uint64_t)tcp_rto_get(&c->rto)) {  // retransmit
            tcp_xmit_seq(c, PSH | ACK, c->sndseq, c->sndbuf, c->sndlen);
            c->snd_retx = 1;
            tcp_rto_backoff(&c->rto);                         // exponential backoff
            last_tx = timer_now_us();
        }
        net_wait(20);
    }
    return n;                              // best effort: assume delivered
}

int tcp_recv(struct tcp_conn *c, void *buf, int len)
{
    if (!c) { return -1; }
    while (ring_empty(c) && !c->peer_fin && !c->reset) {
        net_pump();
        if (!ring_empty(c) || c->peer_fin || c->reset) { break; }
        if (tcp_interrupted()) { return -1; }
        net_wait(20);
    }
    if (!ring_empty(c)) {
        int got = ring_pop(c, buf, len);
        // Window update: draining the ring frees receive space. If our window can
        // now grow by at least an MSS over what we last told the peer, send a bare
        // ACK so it learns it may resume -- without this the peer could stay
        // throttled against a stale, smaller window (silly-window avoidance).
        if (c->state == ESTABLISHED &&
            tcp_advertise_wnd(ring_free(c)) >= c->last_adv + TCP_MSS) {
            tcp_xmit(c, ACK, 0, 0);
        }
        return got;
    }
    if (c->reset) { return -1; }
    return 0;                              // peer closed, ring drained -> EOF
}

void tcp_close(struct tcp_conn *c)
{
    if (!c) { return; }
    if (c->state == ESTABLISHED) {
        tcp_xmit(c, FIN | ACK, 0, 0);
        c->snd_nxt += 1;                   // FIN consumes one sequence number
        c->state = FIN_WAIT;
        uint64_t start = timer_now_us();
        while (timer_now_us() - start < 2000000 && c->state != CLOSED && !c->reset) {
            net_pump();
            net_wait(20);
        }
    }
    c->used = 0;
}
