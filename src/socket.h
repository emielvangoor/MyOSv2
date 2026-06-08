// socket.h -- a tiny BSD-style socket layer (UDP datagrams for now).
// ==================================================================
//
// A socket is an endpoint with a local port and a queue of received datagrams.
// The UDP input path (netstack) delivers each datagram to the socket bound to
// its destination port; a recvfrom() pumps the network and sleeps until one
// arrives. TCP stream sockets are added later on top of the same fd plumbing.
#pragma once
#include <stdint.h>

#define SOCK_DGRAM  1   // UDP datagram socket
#define SOCK_STREAM 2   // TCP stream socket (added later)

struct socket;

struct socket *socket_alloc(int type);                 // 0 if none free
void socket_free(struct socket *s);
int  socket_bind(struct socket *s, uint16_t port);     // 0 ok, -1 if taken
int  socket_sendto(struct socket *s, const void *buf, int len,
                   uint32_t dst_ip, uint16_t dst_port); // bytes sent, -1 on error
// Block until a datagram arrives; fills *src_ip/*src_port. -1 on EINTR.
int  socket_recvfrom(struct socket *s, void *buf, int len,
                     uint32_t *src_ip, uint16_t *src_port);

// Called by the UDP receive path to hand a datagram to a bound socket.
void socket_udp_input(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                      const uint8_t *data, int len);
