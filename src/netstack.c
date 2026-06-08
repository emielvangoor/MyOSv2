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

// ---- IPv4 ----

// Send an IPv4 packet (proto, payload) to dst_ip. Resolves the next hop (the
// gateway unless dst is on our /24) and builds the IPv4 header + checksum.
static int ip_send(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, int len)
{
    uint32_t nexthop = ((dst_ip ^ IP_OURS) & 0xffffff00u) ? IP_GATEWAY : dst_ip;
    uint8_t mac[6];
    if (arp_resolve(nexthop, mac) != 0) { return -1; }

    static uint8_t pkt[1500];
    pkt[0] = 0x45;                       // IPv4, IHL 5 (20 bytes)
    pkt[1] = 0;
    put16(pkt + 2, (uint16_t)(20 + len));// total length
    put16(pkt + 4, 0);                   // id
    put16(pkt + 6, 0);                   // flags/fragment
    pkt[8] = 64;                         // TTL
    pkt[9] = proto;
    put16(pkt + 10, 0);                  // checksum (computed next)
    put32(pkt + 12, IP_OURS);
    put32(pkt + 16, dst_ip);
    put16(pkt + 10, inet_csum(pkt, 20));
    for (int i = 0; i < len; i++) { pkt[20 + i] = payload[i]; }
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

static void icmp_input(uint32_t src_ip, const uint8_t *p, int len);

static void ip_input(const uint8_t *pkt, int len)
{
    if (len < 20) { return; }
    int ihl = (pkt[0] & 0x0f) * 4;
    if (ihl < 20 || ihl > len) { return; }
    uint8_t proto = pkt[9];
    uint32_t src = get32(pkt + 12);
    uint32_t dst = get32(pkt + 16);
    if (dst != IP_OURS) { return; }      // not addressed to us
    int total = get16(pkt + 2);
    if (total > len) { total = len; }
    const uint8_t *payload = pkt + ihl;
    int paylen = total - ihl;
    if (paylen < 0) { return; }

    if (proto == 1) { icmp_input(src, payload, paylen); }
    // proto 17 (UDP) / 6 (TCP) demux are added in later tasks.
}

// ---- ICMP (ping) ----

static volatile int ping_id;
static volatile int ping_got;

static void icmp_input(uint32_t src_ip, const uint8_t *p, int len)
{
    if (len < 8) { return; }
    uint8_t type = p[0];
    if (type == 8) {                     // echo request -> reply (we're pingable)
        static uint8_t reply[1500];
        for (int i = 0; i < len; i++) { reply[i] = p[i]; }
        reply[0] = 0;                    // echo reply
        put16(reply + 2, 0);
        put16(reply + 2, inet_csum(reply, len));
        ip_send(src_ip, 1, reply, len);
    } else if (type == 0) {              // echo reply -> wake our waiter if it matches
        if (get16(p + 4) == (uint16_t)ping_id) { ping_got = 1; }
    }
}

int net_ping(uint32_t ip, int *ms)
{
    static int seq;
    ping_id = (ping_id + 1) & 0xffff;
    ping_got = 0;

    uint8_t icmp[64];
    for (int i = 0; i < 64; i++) { icmp[i] = 0; }
    icmp[0] = 8;                         // echo request
    put16(icmp + 4, (uint16_t)ping_id);
    put16(icmp + 6, (uint16_t)(++seq));
    for (int i = 0; i < 32; i++) { icmp[8 + i] = (uint8_t)i; }   // payload
    put16(icmp + 2, inet_csum(icmp, 40));
    if (ip_send(ip, 1, icmp, 40) != 0) { return -1; }

    extern uint64_t sched_jiffies(void);  // milliseconds since boot (TIMER_HZ=1000)
    uint64_t start = sched_jiffies();
    for (long tries = 0; tries < 50000000L; tries++) {
        net_pump();
        if (ping_got) { if (ms) { *ms = (int)(sched_jiffies() - start); } return 0; }
    }
    return -1;                           // no reply
}

// ---- inbound dispatch ----

static void net_input(const uint8_t *f, int len)
{
    if (len < 14) { return; }
    uint16_t ethertype = get16(f + 12);
    if (ethertype == 0x0806) { arp_input(f, len); }
    else if (ethertype == 0x0800) { ip_input(f + 14, len - 14); }
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
