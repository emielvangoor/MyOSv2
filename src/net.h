// net.h -- the link layer: send and receive raw Ethernet frames via virtio-net.
#pragma once
#include <stdint.h>

void virtio_net_init(void);                 // probe + set up the NIC
int  net_present(void);                     // is a NIC attached?
void net_mac(uint8_t out[6]);               // our hardware (MAC) address
int  net_send(const void *frame, int len);  // transmit one Ethernet frame; 0 ok
int  net_recv(void *buf, int max);          // receive one frame (payload only); 0 if none
