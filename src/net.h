// net.h -- the link layer: send and receive raw Ethernet frames via virtio-net.
#pragma once
#include <stdint.h>

void virtio_net_init(void);                 // probe + set up the NIC
int  net_present(void);                     // is a NIC attached?
void net_mac(uint8_t out[6]);               // our hardware (MAC) address
int  net_send(const void *frame, int len);  // transmit one Ethernet frame; 0 ok
int  net_recv(void *buf, int max);          // receive one frame (payload only); 0 if none

// --- TCP/IP stack (Phase 22). IP addresses are host-order uint32_t. ---
#define IP_OURS    0x0a00020fu   // 10.0.2.15 (QEMU user-net guest address)
#define IP_GATEWAY 0x0a000202u   // 10.0.2.2  (QEMU user-net gateway)

void     net_stack_init(void);                       // initialise the stack
uint16_t inet_csum(const void *buf, int len);        // internet (one's-complement) checksum
int      net_pump(void);                             // process one inbound frame; 1=did, 0=idle
int      arp_resolve(uint32_t ip, uint8_t mac[6]);   // IP -> MAC (yields/pumps); 0 ok, -1 timeout
