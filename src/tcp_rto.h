// tcp_rto.h -- TCP retransmission-timeout (RTO) estimator, RFC 6298.
// ==================================================================
//
// A sender must decide how long to wait for an ACK before assuming a segment
// was lost and resending it. Too short and it floods the network with needless
// retransmits; too long and it stalls after every real loss. RFC 6298 derives
// that timeout from *measured* round-trip times so it adapts to the actual path.
//
// The estimator keeps two smoothed quantities, both in microseconds:
//   SRTT   -- a smoothed average round-trip time
//   RTTVAR -- a smoothed mean deviation (how jittery the RTT is)
// and sets   RTO = SRTT + 4 * RTTVAR,   clamped to [MIN, MAX].
//
// Karn's algorithm lives in the *caller*: never feed tcp_rto_sample() an RTT
// taken from a segment that was retransmitted -- you can't tell whether the ACK
// answered the original or the retransmit, so the sample is ambiguous. Instead,
// on each timeout the caller calls tcp_rto_backoff() to double the timeout.
//
// This is a pure module (no clock, no I/O): the caller measures elapsed time and
// feeds in the numbers, which makes it exhaustively unit-testable.
#pragma once
#include <stdint.h>

#define TCP_RTO_INIT 1000000     // 1 s   -- the initial RTO before any sample (RFC)
#define TCP_RTO_MIN   200000     // 200 ms -- floor (RFC says 1 s; we relax it for a
                                 //           fast emulated LAN so the client stays snappy)
#define TCP_RTO_MAX 60000000     // 60 s  -- ceiling (RFC)

struct tcp_rto {
    int      seeded;             // has the first RTT sample arrived?
    int32_t  srtt;               // smoothed round-trip time (us)
    int32_t  rttvar;             // smoothed RTT variation/deviation (us)
    int32_t  rto;                // current retransmission timeout (us)
};

// Reset to the pre-measurement state: RTO = TCP_RTO_INIT, no samples yet.
void tcp_rto_init(struct tcp_rto *e);

// Feed one round-trip-time measurement `rtt_us` (microseconds). Updates SRTT,
// RTTVAR and recomputes RTO. MUST NOT be called for a retransmitted segment
// (Karn's algorithm -- the caller enforces this).
void tcp_rto_sample(struct tcp_rto *e, int32_t rtt_us);

// A retransmit timeout fired: double the RTO (exponential backoff), capped at
// TCP_RTO_MAX. Leaves SRTT/RTTVAR untouched, so the next clean sample collapses
// the backoff back to the measured estimate.
void tcp_rto_backoff(struct tcp_rto *e);

// The current retransmission timeout, in microseconds.
int32_t tcp_rto_get(const struct tcp_rto *e);
