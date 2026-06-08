// netstack.c -- a small TCP/IP stack on top of virtio-net.
// ========================================================
//
// Frames are processed by POLLING: each blocking call (arp_resolve, ping,
// recvfrom, ...) "pumps" the receive path itself -- net_pump() pulls one frame
// and dispatches it. This suits our cooperative single-core kernel and works in
// the test harness (where the timer isn't running). Layers are added file-by-
// file: this part is Ethernet framing + ARP.

#include <stdint.h>
#include "net.h"

// ---- helpers ----

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// The internet checksum: one's-complement sum of 16-bit words, folded + inverted.
uint16_t inet_csum(const void *buf, int len)
{
    const uint8_t *p = buf;
    uint32_t sum = 0;
    while (len > 1) { sum += get16(p); p += 2; len -= 2; }
    if (len) { sum += (uint32_t)p[0] << 8; }     // odd trailing byte
    while (sum >> 16) { sum = (sum & 0xffff) + (sum >> 16); }
    return (uint16_t)(~sum);
}

// ---- stack state ----

static uint8_t our_mac[6];

struct arp_entry { uint32_t ip; uint8_t mac[6]; int valid; };
static struct arp_entry arp_cache[8];

void net_stack_init(void)
{
    net_mac(our_mac);
    for (int i = 0; i < 8; i++) { arp_cache[i].valid = 0; }
}

// ---- Ethernet ----

static uint8_t txframe[1600];

static int eth_send(const uint8_t dst[6], uint16_t ethertype, const uint8_t *payload, int len)
{
    for (int i = 0; i < 6; i++) { txframe[i] = dst[i]; txframe[6 + i] = our_mac[i]; }
    put16(txframe + 12, ethertype);
    for (int i = 0; i < len; i++) { txframe[14 + i] = payload[i]; }
    return net_send(txframe, 14 + len);
}

// ---- ARP ----

static void arp_cache_put(uint32_t ip, const uint8_t mac[6])
{
    for (int i = 0; i < 8; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) { arp_cache[i].mac[j] = mac[j]; }
            return;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip; arp_cache[i].valid = 1;
            for (int j = 0; j < 6; j++) { arp_cache[i].mac[j] = mac[j]; }
            return;
        }
    }
}

static int arp_cache_get(uint32_t ip, uint8_t out[6])
{
    for (int i = 0; i < 8; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) { out[j] = arp_cache[i].mac[j]; }
            return 1;
        }
    }
    return 0;
}

// Build + send an ARP packet (request or reply). `tha`/`tpa` = target hw/proto.
static void arp_xmit(uint16_t oper, const uint8_t *tha, uint32_t tpa, const uint8_t *dst_mac)
{
    uint8_t a[28];
    put16(a + 0, 1);          // htype = Ethernet
    put16(a + 2, 0x0800);     // ptype = IPv4
    a[4] = 6; a[5] = 4;       // hlen, plen
    put16(a + 6, oper);
    for (int i = 0; i < 6; i++) { a[8 + i] = our_mac[i]; }   // sender hw
    put32(a + 14, IP_OURS);                                  // sender proto
    for (int i = 0; i < 6; i++) { a[18 + i] = tha[i]; }      // target hw
    put32(a + 24, tpa);                                      // target proto
    eth_send(dst_mac, 0x0806, a, 28);
}

static void arp_input(const uint8_t *f, int len)
{
    if (len < 14 + 28) { return; }
    const uint8_t *a = f + 14;
    uint16_t oper = get16(a + 6);
    uint32_t spa = get32(a + 14);           // sender protocol (IP) address
    const uint8_t *sha = a + 8;             // sender hardware (MAC) address
    uint32_t tpa = get32(a + 24);           // target protocol address

    if (oper == 1 && tpa == IP_OURS) {      // a request for us -> reply
        arp_xmit(2, sha, spa, sha);
    }
    if (oper == 2) {                        // a reply -> cache it
        arp_cache_put(spa, sha);
    }
}

// ---- inbound dispatch ----

static void net_input(const uint8_t *f, int len)
{
    if (len < 14) { return; }
    uint16_t ethertype = get16(f + 12);
    if (ethertype == 0x0806) { arp_input(f, len); }
    // IPv4 (0x0800) dispatch is added with the IP layer (Task 2).
}

static uint8_t rxframe[2048];

int net_pump(void)
{
    int n = net_recv(rxframe, sizeof(rxframe));
    if (n > 0) { net_input(rxframe, n); return 1; }
    return 0;
}

int arp_resolve(uint32_t ip, uint8_t mac[6])
{
    if (arp_cache_get(ip, mac)) { return 0; }

    uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    uint8_t zero[6]  = {0,0,0,0,0,0};
    arp_xmit(1, zero, ip, bcast);                   // broadcast "who has ip?"

    for (long tries = 0; tries < 50000000L; tries++) {
        net_pump();
        if (arp_cache_get(ip, mac)) { return 0; }
    }
    return -1;                                       // timed out
}
