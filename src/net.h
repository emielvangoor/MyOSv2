// net.h -- the link layer: send and receive raw Ethernet frames via virtio-net.
#pragma once
#include <stdint.h>

void virtio_net_init(void);                 // probe + set up the NIC
int  net_present(void);                     // is a NIC attached?
void net_mac(uint8_t out[6]);               // our hardware (MAC) address
int  net_send(const void *frame, int len);  // transmit one Ethernet frame; 0 ok
int  net_recv(void *buf, int max);          // receive one frame (payload only); 0 if none
int  net_irq_id(void);                       // the NIC's GIC interrupt id (-1 if none)
void net_irq_ack(void);                      // acknowledge the NIC's interrupt
void net_isr(void);                          // NIC interrupt handler (ack + wake waiters)
void net_wait(unsigned ms);                  // sleep until the NIC interrupt (or ms ticks)

// --- TCP/IP stack (Phase 22). IP addresses are host-order uint32_t. ---
#define IP_OURS    0x0a00020fu   // 10.0.2.15 (QEMU user-net guest address)
#define IP_GATEWAY 0x0a000202u   // 10.0.2.2  (QEMU user-net gateway)
#define IP_DNS     0x0a000203u   // 10.0.2.3  (QEMU user-net DNS responder)

void     net_stack_init(void);                       // initialise the stack
uint16_t inet_csum(const void *buf, int len);        // internet (one's-complement) checksum
int      net_pump(void);                             // process one inbound frame; 1=did, 0=idle
int      arp_resolve(uint32_t ip, uint8_t mac[6]);   // IP -> MAC (yields/pumps); 0 ok, -1 timeout
int      net_ping(uint32_t ip, int *ms);             // ICMP echo; 0 + round-trip ms, -1 on timeout

// --- DNS over UDP. Pure encode/decode helpers (no I/O) are exposed for tests. ---
// dns_build_query: write a standard A-record query for `host` into buf (>=512
// bytes); returns the message length, or -1 if the name doesn't fit.
int      dns_build_query(uint8_t *buf, uint16_t id, const char *host);
// dns_parse_answer: scan a DNS response for the first A record matching `id`;
// returns 0 and stores the 32-bit host-order address in *ip, or -1 on failure.
int      dns_parse_answer(const uint8_t *msg, int len, uint16_t id, uint32_t *ip);
// net_resolve: resolve a hostname to an IPv4 address via the DNS server; 0 ok.
int      net_resolve(const char *host, uint32_t *ip);
// Send a UDP datagram (used by the socket layer). Returns 0 on success.
int      net_udp_send(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                      const void *data, int len);
// Send a raw IPv4 packet with the given protocol (used by TCP). 0 on success.
int      net_ip_send(uint32_t dst_ip, uint8_t proto, const void *payload, int len);
