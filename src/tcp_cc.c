// tcp_cc.c -- the Reno congestion-control law. See tcp_cc.h.  (stub: not yet wired)
#include "tcp_cc.h"

void tcp_cc_init(struct tcp_cc *cc, uint32_t mss)
{
    cc->cwnd = 4 * mss;
    cc->ssthresh = 0x40000000u;     // effectively infinite -> begin in slow start
    cc->dupacks = 0;
    cc->in_recovery = 0;
}

// ssthresh after a loss: half the current window, but never below two segments.
static uint32_t halve(uint32_t cwnd, uint32_t mss)
{
    uint32_t h = cwnd / 2;
    uint32_t floor = 2 * mss;
    return h > floor ? h : floor;
}

void tcp_cc_on_ack(struct tcp_cc *cc, uint32_t mss)
{
    if (cc->in_recovery) {
        // A new ACK ends fast recovery: deflate the inflated window back to
        // ssthresh and resume normal increase.
        cc->cwnd = cc->ssthresh;
        cc->in_recovery = 0;
        cc->dupacks = 0;
        return;
    }
    cc->dupacks = 0;
    if (cc->cwnd < cc->ssthresh) {
        cc->cwnd += mss;                          // slow start: +1 MSS per ACK
    } else {
        uint32_t inc = mss * mss / cc->cwnd;      // congestion avoidance: ~+1 MSS/RTT
        if (inc == 0) { inc = 1; }
        cc->cwnd += inc;
    }
}

int tcp_cc_on_dupack(struct tcp_cc *cc, uint32_t mss)
{
    if (cc->in_recovery) {
        cc->cwnd += mss;                          // inflate for each dupack in recovery
        return 0;
    }
    if (++cc->dupacks == 3) {                     // three dupacks -> fast retransmit
        cc->ssthresh = halve(cc->cwnd, mss);
        cc->cwnd = cc->ssthresh + 3 * mss;        // fast recovery
        cc->in_recovery = 1;
        return 1;
    }
    return 0;
}

void tcp_cc_on_timeout(struct tcp_cc *cc, uint32_t mss)
{
    // A timeout means the pipe likely drained: be conservative -- drop to one
    // segment and slow-start again from a halved threshold.
    cc->ssthresh = halve(cc->cwnd, mss);
    cc->cwnd = mss;
    cc->dupacks = 0;
    cc->in_recovery = 0;
}

uint32_t tcp_cc_cwnd(const struct tcp_cc *cc)
{
    return cc->cwnd;
}
