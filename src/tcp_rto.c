// tcp_rto.c -- RFC 6298 retransmission-timeout estimator. See tcp_rto.h.
#include "tcp_rto.h"

void tcp_rto_init(struct tcp_rto *e)
{
    e->seeded = 0;
    e->srtt = 0;
    e->rttvar = 0;
    e->rto = TCP_RTO_INIT;
}

// Recompute RTO = SRTT + 4*RTTVAR, clamped to [MIN, MAX].
static void recompute(struct tcp_rto *e)
{
    int32_t rto = e->srtt + 4 * e->rttvar;
    if (rto < TCP_RTO_MIN) { rto = TCP_RTO_MIN; }
    if (rto > TCP_RTO_MAX) { rto = TCP_RTO_MAX; }
    e->rto = rto;
}

void tcp_rto_sample(struct tcp_rto *e, int32_t rtt_us)
{
    if (rtt_us < 0) { rtt_us = 0; }
    if (!e->seeded) {
        // First measurement (RFC 6298 §2.2): SRTT = R, RTTVAR = R/2.
        e->srtt   = rtt_us;
        e->rttvar = rtt_us / 2;
        e->seeded = 1;
    } else {
        // Subsequent (RFC 6298 §2.3), beta=1/4, alpha=1/8. RTTVAR is updated
        // first, using the *old* SRTT, both via the standard shift forms:
        //   RTTVAR = 3/4 RTTVAR + 1/4 |SRTT - R|
        //   SRTT   = 7/8 SRTT   + 1/8 R
        int32_t d = e->srtt - rtt_us;
        if (d < 0) { d = -d; }
        e->rttvar = e->rttvar - (e->rttvar >> 2) + (d >> 2);
        e->srtt   = e->srtt   - (e->srtt   >> 3) + (rtt_us >> 3);
    }
    recompute(e);
}

void tcp_rto_backoff(struct tcp_rto *e)
{
    int32_t rto = e->rto * 2;
    if (rto > TCP_RTO_MAX || rto < 0 /* overflow guard */) { rto = TCP_RTO_MAX; }
    e->rto = rto;
}

int32_t tcp_rto_get(const struct tcp_rto *e)
{
    return e->rto;
}
