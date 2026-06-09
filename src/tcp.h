// tcp.h -- a minimal TCP client (connect / send / recv / close).
// ==============================================================
//
// Just enough TCP to be a client on a reliable path (QEMU user-net): a 3-way
// handshake, cumulative ACKs, and a FIN close. Out-of-order segments are
// reassembled (tcp_reasm.h) so a dropped packet no longer discards the rest of
// the stream; the single outstanding segment is retransmitted on an adaptive RTO
// (RFC 6298, tcp_rto.h) with Karn's algorithm + exponential backoff; and flow
// control is honored both ways -- we never send past the peer's advertised window
// and advertise our own from real receive-buffer space -- Reno congestion control
// (tcp_cc.h) bounds in-flight data by a congestion window, and a write larger than
// one MSS is split into segments pipelined up to that window (with Nagle). No
// window scaling yet -- a corner a real stack handles but a one-page client skips.
#pragma once
#include <stdint.h>

struct tcp_conn;

struct tcp_conn *tcp_new(void);                                   // allocate a connection
int  tcp_connect(struct tcp_conn *c, uint32_t ip, uint16_t port); // 3-way handshake; 0 ok
int  tcp_send(struct tcp_conn *c, const void *buf, int len);      // bytes sent, -1 on error
int  tcp_recv(struct tcp_conn *c, void *buf, int len);            // bytes (0 = peer closed), -1 err
void tcp_close(struct tcp_conn *c);                               // FIN + free

// --- readiness for poll() + half-close (Phase 23.5) ---
int  tcp_readable(struct tcp_conn *c);   // recv won't block (data queued, or peer closed/reset)
int  tcp_writable(struct tcp_conn *c);   // established with an open send window
void tcp_shutdown(struct tcp_conn *c);   // send FIN (half-close write side); keep reading

// --- passive open (server side, Phase 23.4) ---
int  tcp_listen(struct tcp_conn *c, uint16_t port);   // become a listener on `port`; 0 ok
// Block until an inbound connection completes its handshake, then return the new
// (ESTABLISHED) connection. Returns 0 on EINTR. The listener stays open for more.
struct tcp_conn *tcp_accept(struct tcp_conn *listener);

// The TCP receive path (called from ip_input for protocol 6).
void tcp_input(uint32_t src_ip, const uint8_t *seg, int len);

// Compute the seq/ack/flags of a RST reply to a segment that matched no
// connection (RFC 793). Exposed for tests. (flag bits: SYN=0x02, RST=0x04,
// ACK=0x10.)
void tcp_rst_fields(unsigned char in_flags, uint32_t in_seq, uint32_t in_ack,
                    int seg_len, uint32_t *seq, uint32_t *ack, unsigned char *flags);

// The TCP checksum over the IPv4 pseudo-header + segment (exposed for tests).
uint16_t tcp_checksum(uint32_t sip, uint32_t dip, const uint8_t *seg, int len);

// --- Flow-control window arithmetic (Phase 23.3; exposed for tests) ---
// tcp_advertise_wnd: the receive window to advertise to the peer given how many
// bytes of receive-buffer space are currently free -- capped to what we can
// actually buffer ahead and to the 16-bit window field.
uint16_t tcp_advertise_wnd(int free_bytes);
// tcp_window_avail: how many new bytes we may transmit given the oldest unacked
// sequence number, the next send sequence number, the peer's advertised window,
// and the MSS. Zero means the window is closed (don't send / probe instead).
int tcp_window_avail(uint32_t snd_una, uint32_t snd_nxt, uint16_t peer_wnd, int mss);

// tcp_next_seg: how many bytes to put in the next segment when streaming a write,
// given the unsent bytes remaining, the bytes already in flight, the effective
// window (min of peer + congestion windows), and the MSS. Returns 0 to wait --
// either the window is full, or Nagle/silly-window avoidance is holding a
// sub-MSS segment because data is already in flight. (Exposed for tests.)
int tcp_next_seg(int unsent, int inflight, int win, int mss);
