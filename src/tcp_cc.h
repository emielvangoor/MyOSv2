// tcp_cc.h -- TCP congestion control (the Reno control law, RFC 5681).
// ====================================================================
//
// Flow control (23.3) keeps a sender from overrunning the *receiver*. Congestion
// control keeps it from overrunning the *network* -- the routers and links in
// between, which have no window field to advertise their limits. The sender must
// instead infer the path's capacity from feedback (ACKs and losses) and keep a
// second, self-imposed limit on data in flight: the congestion window, cwnd.
//
// Reno is the classic scheme:
//   * Slow start -- cwnd below ssthresh: probe exponentially, +1 MSS per ACK
//     (doubling cwnd each round-trip) until a loss or ssthresh is reached.
//   * Congestion avoidance -- cwnd at/above ssthresh: probe linearly, ~+1 MSS per
//     round-trip (additive increase).
//   * Loss is the congestion signal. A retransmit timeout is treated as severe:
//     ssthresh = cwnd/2, cwnd = 1 MSS (back to slow start). Three duplicate ACKs
//     are treated as mild (one packet lost, later ones still arriving): halve and
//     fast-retransmit without collapsing cwnd to one -- "fast recovery".
//
// The effective amount a sender may have outstanding is min(cwnd, peer window).
//
// This is a pure module -- no clock, no sockets -- so the whole control law is
// unit-testable. tcp.c owns one per connection and feeds it ACK/loss events.
#pragma once
#include <stdint.h>

struct tcp_cc {
    uint32_t cwnd;        // congestion window (bytes)
    uint32_t ssthresh;    // slow-start threshold (bytes)
    int      dupacks;     // consecutive duplicate ACKs seen
    int      in_recovery; // fast-recovery in progress
};

// Initialise: cwnd = 4*mss (a modest RFC 5681 initial window), ssthresh "infinite"
// so we begin in slow start.
void tcp_cc_init(struct tcp_cc *cc, uint32_t mss);

// A cumulative ACK acknowledged new data. Grows cwnd (slow start or congestion
// avoidance) and exits fast recovery if it was active.
void tcp_cc_on_ack(struct tcp_cc *cc, uint32_t mss);

// A duplicate ACK arrived (no new data acked). On the third, halve ssthresh,
// enter fast recovery, and return 1 to signal "fast-retransmit now"; otherwise
// inflate cwnd during recovery and return 0.
int  tcp_cc_on_dupack(struct tcp_cc *cc, uint32_t mss);

// A retransmission timeout fired (severe loss): ssthresh = cwnd/2, cwnd = 1 MSS.
void tcp_cc_on_timeout(struct tcp_cc *cc, uint32_t mss);

// The current congestion window (bytes).
uint32_t tcp_cc_cwnd(const struct tcp_cc *cc);
