// tcp.h -- a minimal TCP client (connect / send / recv / close).
// ==============================================================
//
// Just enough TCP to be a client on a reliable path (QEMU user-net): a 3-way
// handshake, cumulative ACKs, a FIN close, and a simple retransmit of unacked
// segments. Out-of-order segments are reassembled (see tcp_reasm.h) so a dropped
// packet no longer discards the rest of the stream. No window scaling yet -- a
// corner a real stack handles but a fetch-one-page client can skip.
#pragma once
#include <stdint.h>

struct tcp_conn;

struct tcp_conn *tcp_new(void);                                   // allocate a connection
int  tcp_connect(struct tcp_conn *c, uint32_t ip, uint16_t port); // 3-way handshake; 0 ok
int  tcp_send(struct tcp_conn *c, const void *buf, int len);      // bytes sent, -1 on error
int  tcp_recv(struct tcp_conn *c, void *buf, int len);            // bytes (0 = peer closed), -1 err
void tcp_close(struct tcp_conn *c);                               // FIN + free

// The TCP receive path (called from ip_input for protocol 6).
void tcp_input(uint32_t src_ip, const uint8_t *seg, int len);

// The TCP checksum over the IPv4 pseudo-header + segment (exposed for tests).
uint16_t tcp_checksum(uint32_t sip, uint32_t dip, const uint8_t *seg, int len);
