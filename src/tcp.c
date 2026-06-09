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
#define TCP_MSS   1400
#define TCP_WIN   8192          // advertised receive window (<= ring capacity)

struct tcp_conn {
    int      used;
    int      state;
    uint16_t lport, rport;
    uint32_t rip;
    uint32_t snd_nxt;           // next sequence number to send
    uint32_t snd_una;           // oldest unacknowledged sequence number
    uint32_t rcv_nxt;           // next sequence number expected from the peer
    uint8_t  rx[TCP_RXBUF];     // received-data ring (in-order bytes, app-visible)
    int      rxhead, rxtail;
    struct tcp_reasm reasm;     // holds out-of-order segments until the gap fills
    int      have_fin;          // peer's FIN has been seen (maybe out of order)...
    uint32_t fin_seq;           // ...at this sequence number (consumed when in order)
    int      peer_fin;          // FIN now in order -> EOF once the ring drains
    int      reset;             // peer sent RST
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
    put16(seg + 14, TCP_WIN);
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
            c->state = ESTABLISHED;
            tcp_reasm_init(&c->reasm, c->rcv_nxt);   // start reassembly from here
            tcp_xmit(c, ACK, 0, 0);        // finish the handshake
        }
        return;
    }

    if (c->state == ESTABLISHED || c->state == FIN_WAIT) {
        if ((flags & ACK) && seq_gt(ack, c->snd_una)) { c->snd_una = ack; }

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
    while (timer_now_us() - start < 5000000) {     // 5 s
        net_pump();
        if (c->state == ESTABLISHED) { return 0; }
        if (c->reset) { return -1; }
        if (tcp_interrupted()) { return -1; }
        if (timer_now_us() - last_tx > 500000) {   // retransmit the SYN
            tcp_xmit_seq(c, SYN, isn, 0, 0);
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
    const uint8_t *data = buf;

    uint32_t seq = c->snd_nxt;
    tcp_xmit_seq(c, PSH | ACK, seq, data, len);
    c->snd_nxt = seq + (uint32_t)len;

    uint64_t start = timer_now_us(), last_tx = start;
    while (timer_now_us() - start < 5000000) {
        net_pump();
        if (seq_ge(c->snd_una, c->snd_nxt)) { return len; }   // fully acked
        if (c->reset) { return -1; }
        if (tcp_interrupted()) { return -1; }
        if (timer_now_us() - last_tx > 500000) {              // retransmit
            tcp_xmit_seq(c, PSH | ACK, seq, data, len);
            last_tx = timer_now_us();
        }
        net_wait(20);
    }
    return len;                            // best effort: assume delivered
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
    if (!ring_empty(c)) { return ring_pop(c, buf, len); }
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
