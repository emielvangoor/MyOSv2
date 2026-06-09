// netstack.c -- a small TCP/IP stack on top of virtio-net.
// ========================================================
//
// Layers, bottom-up: Ethernet framing, ARP, IPv4 (+ checksum + routing), ICMP
// (ping), UDP, and a DNS resolver. A blocking call (arp_resolve, net_ping,
// net_resolve) sends, then SLEEPS on the NIC wait-channel (net_wait) until the
// receive interrupt wakes it; net_pump() then pulls the frame and dispatches it
// up the stack. A pending signal aborts the wait (EINTR -> Ctrl-C). During the
// boot self-tests (no IRQs yet) net_wait() is a no-op so the loops just poll.

#include <stdint.h>
#include "net.h"
#include "timer.h"
#include "sched.h"
#include "socket.h"
#include "tcp.h"

// How long a blocking request (ARP, ping, DNS) waits for its reply before giving
// up. Timed against the free-running hardware counter, so it's real wall-clock
// time regardless of scheduling.
#define NET_TIMEOUT_US 2000000   // 2 seconds

// net_isr / net_wait live in the driver (virtio_net.c) since the wait-channel is
// tied to the NIC interrupt; receivers here just call net_wait() to sleep on it.

// Has a signal been posted to us? A blocking network call returns early (EINTR)
// so e.g. Ctrl-C can abandon a ping.
static int net_signal_pending(void)
{
    struct thread *t = sched_current();
    return t && t->sig_pending;
}

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

static uint8_t  our_mac[6];
// These default to QEMU user-net's values and are replaced by whatever DHCP leases
// us (see net_dhcp / dhcp_apply). Keeping them runtime is what lets the OS work on
// a network other than SLIRP.
static uint32_t our_ip      = IP_OURS;
static uint32_t our_gateway = IP_GATEWAY;
static uint32_t our_dns     = IP_DNS;
static uint32_t our_mask    = 0xffffff00u;   // /24

uint32_t net_our_ip(void)  { return our_ip; }
void     net_set_ip(uint32_t ip) { our_ip = ip; }
uint32_t net_gateway(void) { return our_gateway; }
uint32_t net_dns(void)     { return our_dns; }
uint32_t net_mask(void)    { return our_mask; }

struct arp_entry { uint32_t ip; uint8_t mac[6]; int valid; };
static struct arp_entry arp_cache[8];

void net_stack_init(void)
{
    net_mac(our_mac);
    for (int i = 0; i < 8; i++) { arp_cache[i].valid = 0; }
}

// The UDP checksum: like the TCP one, a one's-complement sum over an IPv4
// pseudo-header (src, dst, proto=17, UDP length) plus the UDP header + data.
uint16_t udp_checksum(uint32_t sip, uint32_t dip, const uint8_t *seg, int len)
{
    uint32_t sum = 0;
    sum += (sip >> 16) & 0xffff; sum += sip & 0xffff;
    sum += (dip >> 16) & 0xffff; sum += dip & 0xffff;
    sum += 17;                              // protocol
    sum += (uint32_t)len;                   // UDP length
    for (int i = 0; i + 1 < len; i += 2) { sum += (uint32_t)((seg[i] << 8) | seg[i + 1]); }
    if (len & 1) { sum += (uint32_t)(seg[len - 1] << 8); }
    while (sum >> 16) { sum = (sum & 0xffff) + (sum >> 16); }
    uint16_t c = (uint16_t)~sum;
    return c ? c : 0xffff;                  // 0 would mean "no checksum"; send 0xffff
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
    put32(a + 14, our_ip);                                  // sender proto
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

    if (oper == 1 && tpa == our_ip) {      // a request for us -> reply
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
    // On-link if dst shares our network (per the subnet mask); otherwise via the
    // default gateway. Both come from DHCP (or the built-in defaults).
    uint32_t nexthop = ((dst_ip ^ our_ip) & our_mask) ? our_gateway : dst_ip;
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
    put32(pkt + 12, our_ip);
    put32(pkt + 16, dst_ip);
    put16(pkt + 10, inet_csum(pkt, 20));
    for (int i = 0; i < len; i++) { pkt[20 + i] = payload[i]; }
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

static void icmp_input(uint32_t src_ip, const uint8_t *p, int len);
static void udp_input(uint32_t src_ip, const uint8_t *p, int len);

// Public IP send (TCP's transmit path).
int net_ip_send(uint32_t dst_ip, uint8_t proto, const void *payload, int len)
{
    return ip_send(dst_ip, proto, (const uint8_t *)payload, len);
}

static void ip_input(const uint8_t *pkt, int len)
{
    if (len < 20) { return; }
    int ihl = (pkt[0] & 0x0f) * 4;
    if (ihl < 20 || ihl > len) { return; }
    uint8_t proto = pkt[9];
    uint32_t src = get32(pkt + 12);
    uint32_t dst = get32(pkt + 16);
    // Accept packets addressed to us, or broadcast (255.255.255.255) so the DHCP
    // client can receive an OFFER/ACK before we have an address of our own.
    if (dst != our_ip && dst != 0xffffffffu) { return; }
    int total = get16(pkt + 2);
    if (total > len) { total = len; }
    const uint8_t *payload = pkt + ihl;
    int paylen = total - ihl;
    if (paylen < 0) { return; }

    if (proto == 1)       { icmp_input(src, payload, paylen); }
    else if (proto == 17) { udp_input(src, payload, paylen); }
    else if (proto == 6)  { tcp_input(src, payload, paylen); }
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

    // Wait for the echo reply: process any delivered frames, then SLEEP until the
    // NIC interrupt wakes us (or a signal, or the timeout). Round-trip time comes
    // from the free-running hardware counter.
    uint64_t start = timer_now_us();
    while (timer_now_us() - start < NET_TIMEOUT_US) {
        net_pump();
        if (ping_got) {
            if (ms) { *ms = (int)((timer_now_us() - start) / 1000); }   // -> ms
            return 0;
        }
        if (net_signal_pending()) { return -1; }    // EINTR (e.g. Ctrl-C)
        net_wait(20);
    }
    return -1;                           // no reply within the timeout
}

// ---- DNS (over UDP) ----

// Encode a hostname as DNS QNAME labels: "a.bc" -> [1]'a'[2]'b''c'[0]. Each dot-
// separated label is length-prefixed; a zero byte ends the name. Returns the
// number of bytes written, or -1 if a label is malformed or it doesn't fit.
static int dns_encode_name(uint8_t *out, int cap, const char *host)
{
    int w = 0;
    const char *p = host;
    while (*p) {
        const char *start = p;
        while (*p && *p != '.') { p++; }
        int label = (int)(p - start);
        if (label == 0 || label > 63) { return -1; }   // empty/oversized label
        if (w + 1 + label >= cap) { return -1; }
        out[w++] = (uint8_t)label;
        for (int i = 0; i < label; i++) { out[w++] = (uint8_t)start[i]; }
        if (*p == '.') { p++; }
    }
    if (w + 1 > cap) { return -1; }
    out[w++] = 0;                                       // root label terminates
    return w;
}

int dns_build_query(uint8_t *buf, uint16_t id, const char *host)
{
    put16(buf + 0, id);
    put16(buf + 2, 0x0100);     // QR=0 (query), RD=1 (recursion desired)
    put16(buf + 4, 1);          // QDCOUNT = 1 question
    put16(buf + 6, 0);          // ANCOUNT
    put16(buf + 8, 0);          // NSCOUNT
    put16(buf + 10, 0);         // ARCOUNT
    int n = dns_encode_name(buf + 12, 512 - 12 - 4, host);
    if (n < 0) { return -1; }
    int w = 12 + n;
    put16(buf + w, 1); w += 2;  // QTYPE  = A (host address)
    put16(buf + w, 1); w += 2;  // QCLASS = IN (internet)
    return w;
}

// Advance past a DNS name beginning at msg+o. Names are sequences of length-
// prefixed labels ending in a zero byte, but a label whose top two bits are set
// is a 2-byte "compression pointer" to a name elsewhere -- which terminates the
// encoding here. Returns the offset just past the name, or -1 on overrun.
static int dns_skip_name(const uint8_t *msg, int len, int o)
{
    while (o < len) {
        uint8_t b = msg[o];
        if ((b & 0xc0) == 0xc0) { return o + 2; }       // compression pointer
        if (b == 0) { return o + 1; }                   // root label
        o += 1 + b;                                      // ordinary label
    }
    return -1;
}

int dns_parse_answer(const uint8_t *msg, int len, uint16_t id, uint32_t *ip)
{
    if (len < 12) { return -1; }
    if (get16(msg + 0) != id) { return -1; }            // not our transaction
    if ((get16(msg + 2) & 0x000f) != 0) { return -1; }  // RCODE != 0 -> error
    int qd = get16(msg + 4);
    int an = get16(msg + 6);

    int o = 12;
    for (int i = 0; i < qd; i++) {                       // skip the question(s)
        o = dns_skip_name(msg, len, o);
        if (o < 0 || o + 4 > len) { return -1; }
        o += 4;                                          // QTYPE + QCLASS
    }
    for (int i = 0; i < an; i++) {                       // scan the answer(s)
        o = dns_skip_name(msg, len, o);
        if (o < 0 || o + 10 > len) { return -1; }
        uint16_t type  = get16(msg + o);
        uint16_t rdlen = get16(msg + o + 8);
        o += 10;                                         // TYPE+CLASS+TTL+RDLENGTH
        if (o + rdlen > len) { return -1; }
        if (type == 1 && rdlen == 4) {                   // an A record: done
            if (ip) { *ip = get32(msg + o); }
            return 0;
        }
        o += rdlen;                                      // CNAME/other -> keep going
    }
    return -1;                                           // no A record present
}

// ---- UDP ----
//
// Just enough UDP to carry DNS: a connectionless datagram with an 8-byte header
// (source port, dest port, length, checksum). The IPv4 UDP checksum is optional,
// so we send 0 (= "not computed") -- valid and what many minimal stacks do.

static int udp_send(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                    const uint8_t *payload, int len)
{
    static uint8_t seg[1500];
    put16(seg + 0, sport);
    put16(seg + 2, dport);
    put16(seg + 4, (uint16_t)(8 + len));     // UDP length = header + data
    put16(seg + 6, 0);                       // checksum field zero while we compute it
    for (int i = 0; i < len; i++) { seg[8 + i] = payload[i]; }
    put16(seg + 6, udp_checksum(our_ip, dst_ip, seg, 8 + len));   // now fill it in
    return ip_send(dst_ip, 17, seg, 8 + len);
}

// A single outstanding DNS request, captured by the inbound path. The poller in
// net_resolve sets dns_waiting/dns_sport, then spins net_pump() until dns_got.
static volatile int dns_waiting;
static uint16_t     dns_sport;
static uint8_t      dns_rxbuf[512];
static volatile int dns_rxlen;
static volatile int dns_got;

// A single outstanding DHCP exchange, captured by udp_input (port 68).
static volatile int dhcp_waiting;
static uint8_t      dhcp_rxbuf[576];
static volatile int dhcp_rxlen;
static volatile int dhcp_got;

// Public UDP send -- the socket layer's transmit path.
int net_udp_send(uint32_t dst_ip, uint16_t sport, uint16_t dport, const void *data, int len)
{
    return udp_send(dst_ip, sport, dport, (const uint8_t *)data, len);
}

static void udp_input(uint32_t src_ip, const uint8_t *p, int len)
{
    if (len < 8) { return; }
    uint16_t sport = get16(p + 0);
    uint16_t dport = get16(p + 2);
    int ulen = get16(p + 4);
    if (ulen < 8 || ulen > len) { ulen = len; }   // trust the smaller length
    const uint8_t *data = p + 8;
    int dlen = ulen - 8;
    if (dlen < 0) { return; }

    if (dns_waiting && dport == dns_sport && dlen > 0) {   // our internal DNS reply
        int n = dlen > (int)sizeof(dns_rxbuf) ? (int)sizeof(dns_rxbuf) : dlen;
        for (int i = 0; i < n; i++) { dns_rxbuf[i] = data[i]; }
        dns_rxlen = n;
        dns_got = 1;
    }
    if (dhcp_waiting && dport == 68 && dlen > 0) {         // a DHCP OFFER/ACK for us
        int n = dlen > (int)sizeof(dhcp_rxbuf) ? (int)sizeof(dhcp_rxbuf) : dlen;
        for (int i = 0; i < n; i++) { dhcp_rxbuf[i] = data[i]; }
        dhcp_rxlen = n;
        dhcp_got = 1;
    }
    // Hand it to any user socket bound to this port.
    socket_udp_input(src_ip, sport, dport, data, dlen);
}

int net_resolve(const char *host, uint32_t *ip)
{
    static uint16_t qid;
    static uint16_t next_sport = 50000;       // ephemeral source-port cursor
    uint16_t id = ++qid;
    dns_sport = next_sport++;
    if (next_sport == 0) { next_sport = 50000; }
    dns_waiting = 1; dns_got = 0; dns_rxlen = 0;

    uint8_t query[512];
    int qn = dns_build_query(query, id, host);
    if (qn < 0) { dns_waiting = 0; return -1; }
    if (udp_send(our_dns, dns_sport, 53, query, qn) != 0) { dns_waiting = 0; return -1; }

    uint64_t start = timer_now_us();
    while (timer_now_us() - start < NET_TIMEOUT_US) {
        net_pump();
        if (dns_got || net_signal_pending()) { break; }
        net_wait(20);
    }
    dns_waiting = 0;
    if (!dns_got) { return -1; }              // timed out / interrupted
    return dns_parse_answer(dns_rxbuf, dns_rxlen, id, ip);
}

// ---- DHCP client (Phase 23.9 + lease renewal) ----
//
// DHCP rides on BOOTP/UDP (client port 68 -> server port 67): a 236-byte fixed
// header, a magic cookie, then TLV options. The initial lease is the classic
// four-message handshake -- broadcast DISCOVER, receive OFFER, broadcast REQUEST,
// receive ACK -- after which we adopt the address AND apply the gateway/DNS/mask
// the server handed us. We then track the lease clock (RFC 2131): at T1 (half the
// lease) we unicast a RENEW to the server; at T2 (7/8) we broadcast a REBIND; past
// expiry we re-acquire from scratch. Renewal is *activity-driven* -- checked from
// net_pump() in the calling thread (no background net thread, so no new
// concurrency on the shared buffers).

#define DHCP_COOKIE 0x63825363u

// The current lease's clock (microseconds, measured against the free-running
// hardware counter so it works before the scheduler is up). lease_valid gates the
// renewal machinery; in_renew guards net_pump re-entry while a renewal runs.
static int      lease_valid;
static uint64_t lease_t0;          // when the lease was (re)acquired
static uint64_t lease_dur_us;      // total lease duration
static uint32_t lease_server;      // server identifier (for unicast renewals)
static int      in_renew;

int dhcp_parse(const uint8_t *msg, int len, uint32_t xid, struct dhcp_lease *out)
{
    if (len < 240) { return -1; }
    if (msg[0] != 2) { return -1; }                  // op = BOOTREPLY
    if (get32(msg + 4) != xid) { return -1; }        // our transaction id
    if (get32(msg + 236) != DHCP_COOKIE) { return -1; }

    out->type = -1;
    out->yiaddr = get32(msg + 16);                   // offered/leased address
    out->server = out->mask = out->router = out->dns = 0;
    out->lease_secs = 0;
    int o = 240;
    while (o < len) {
        uint8_t code = msg[o++];
        if (code == 255) { break; }                  // end of options
        if (code == 0) { continue; }                 // pad
        if (o >= len) { break; }
        int olen = msg[o++];
        if (o + olen > len) { break; }
        switch (code) {
            case 53: if (olen >= 1) { out->type = msg[o]; } break;        // message type
            case 54: if (olen >= 4) { out->server = get32(msg + o); } break; // server id
            case 1:  if (olen >= 4) { out->mask = get32(msg + o); } break;   // subnet mask
            case 3:  if (olen >= 4) { out->router = get32(msg + o); } break; // router
            case 6:  if (olen >= 4) { out->dns = get32(msg + o); } break;    // DNS server
            case 51: if (olen >= 4) { out->lease_secs = get32(msg + o); } break; // lease
        }
        o += olen;
    }
    return 0;
}

int dhcp_lease_action(uint64_t elapsed_us, uint64_t t1_us, uint64_t t2_us, uint64_t expiry_us)
{
    if (elapsed_us >= expiry_us) { return DHCP_REACQUIRE; }
    if (elapsed_us >= t2_us)     { return DHCP_REBIND; }
    if (elapsed_us >= t1_us)     { return DHCP_RENEW; }
    return DHCP_HOLD;
}

// Build a DHCP message and send it. `bcast` selects broadcast (src 0.0.0.0 ->
// 255.255.255.255, used for DISCOVER/REBIND/initial-REQUEST) vs unicast to the
// server via our address (used for renewals). `ciaddr` is our current address for
// renew/rebind (0 otherwise); options 50/54 are added when reqip/server are set.
static void dhcp_send_msg(int type, uint32_t xid, uint32_t ciaddr,
                          uint32_t reqip, uint32_t server, int bcast)
{
    uint8_t m[300];
    for (unsigned i = 0; i < sizeof(m); i++) { m[i] = 0; }
    m[0] = 1; m[1] = 1; m[2] = 6;            // op=BOOTREQUEST, htype=Ethernet, hlen=6
    put32(m + 4, xid);
    put16(m + 10, bcast ? 0x8000 : 0);       // ask the server to broadcast its reply?
    put32(m + 12, ciaddr);                   // ciaddr (our address when renewing)
    net_mac(m + 28);                         // chaddr = our MAC
    put32(m + 236, DHCP_COOKIE);
    int o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = (uint8_t)type;       // option 53: message type
    if (reqip)  { m[o++] = 50; m[o++] = 4; put32(m + o, reqip);  o += 4; }  // requested IP
    if (server) { m[o++] = 54; m[o++] = 4; put32(m + o, server); o += 4; }  // server id
    m[o++] = 255;                                          // end

    // Wrap in UDP (68->67) then IPv4. Broadcast: 0.0.0.0 -> 255.255.255.255 to a
    // broadcast MAC. Unicast renewal: our address -> the server.
    uint32_t src = bcast ? 0 : our_ip;
    uint32_t dst = bcast ? 0xffffffffu : server;

    uint8_t udp[8 + sizeof(m)];
    put16(udp + 0, 68); put16(udp + 2, 67);
    put16(udp + 4, (uint16_t)(8 + o));
    put16(udp + 6, 0);
    for (int i = 0; i < o; i++) { udp[8 + i] = m[i]; }
    put16(udp + 6, udp_checksum(src, dst, udp, 8 + o));

    if (bcast) {
        uint8_t pkt[20 + 8 + sizeof(m)];
        pkt[0] = 0x45; pkt[1] = 0;
        put16(pkt + 2, (uint16_t)(20 + 8 + o));
        put16(pkt + 4, 0); put16(pkt + 6, 0);
        pkt[8] = 64; pkt[9] = 17; put16(pkt + 10, 0);
        put32(pkt + 12, src); put32(pkt + 16, dst);
        put16(pkt + 10, inet_csum(pkt, 20));
        for (int i = 0; i < 8 + o; i++) { pkt[20 + i] = udp[i]; }
        uint8_t bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
        eth_send(bc, 0x0800, pkt, 20 + 8 + o);
    } else {
        ip_send(dst, 17, udp, 8 + o);        // unicast renewal goes through normal routing
    }
}

// Adopt a lease from an ACK: set our address and the gateway/DNS/mask it carried,
// and start the lease clock.
static void dhcp_apply(const struct dhcp_lease *L)
{
    net_set_ip(L->yiaddr);
    if (L->mask)   { our_mask = L->mask; }
    if (L->router) { our_gateway = L->router; }
    if (L->dns)    { our_dns = L->dns; }
    if (L->server) { lease_server = L->server; }
    lease_dur_us = (uint64_t)(L->lease_secs ? L->lease_secs : 3600) * 1000000ull;
    lease_t0 = timer_now_us();
    lease_valid = 1;
}

// Wait for a DHCP reply of `want_type` matching `xid`; fill *out.
static int dhcp_wait_reply(uint32_t xid, int want_type, struct dhcp_lease *out)
{
    uint64_t start = timer_now_us();
    while (timer_now_us() - start < NET_TIMEOUT_US) {
        dhcp_got = 0;
        net_pump();
        if (dhcp_got && dhcp_parse(dhcp_rxbuf, dhcp_rxlen, xid, out) == 0 &&
            out->type == want_type) {
            return 0;
        }
        if (net_signal_pending()) { return -1; }
        net_wait(20);
    }
    return -1;
}

int net_dhcp(void)
{
    uint32_t xid = (uint32_t)timer_now_us();      // a transaction id for this exchange
    struct dhcp_lease offer, ack;
    dhcp_waiting = 1; dhcp_got = 0;

    dhcp_send_msg(1, xid, 0, 0, 0, 1);             // broadcast DISCOVER
    if (dhcp_wait_reply(xid, 2, &offer) != 0) { dhcp_waiting = 0; return -1; }

    // broadcast REQUEST for the offered address (selecting state: option 50 + 54)
    dhcp_send_msg(3, xid, 0, offer.yiaddr, offer.server, 1);
    if (dhcp_wait_reply(xid, 5, &ack) != 0) { dhcp_waiting = 0; return -1; }

    dhcp_waiting = 0;
    dhcp_apply(&ack);
    return 0;
}

// Send a RENEW (unicast to the server) or REBIND (broadcast) and wait for the ACK,
// re-applying the refreshed lease on success. Returns 0 if the lease was renewed.
static int dhcp_renew(int rebind)
{
    uint32_t xid = (uint32_t)timer_now_us();
    struct dhcp_lease ack;
    dhcp_waiting = 1; dhcp_got = 0;
    // REQUEST with ciaddr = our address, no option 50; server id only when renewing.
    dhcp_send_msg(3, xid, our_ip, 0, rebind ? 0 : lease_server, rebind);
    int rc = dhcp_wait_reply(xid, 5, &ack);
    dhcp_waiting = 0;
    if (rc == 0) { dhcp_apply(&ack); }
    return rc;
}

// Activity-driven lease maintenance: called from net_pump in the caller's thread.
// At T1 renew (unicast), at T2 rebind (broadcast), past expiry re-acquire. The
// in_renew guard stops the renewal's own net_pump() calls from recursing here.
static void dhcp_renew_check(void)
{
    if (!lease_valid || in_renew || dhcp_waiting) { return; }
    uint64_t elapsed = timer_now_us() - lease_t0;
    uint64_t t1 = lease_dur_us / 2;
    uint64_t t2 = lease_dur_us * 7 / 8;
    int action = dhcp_lease_action(elapsed, t1, t2, lease_dur_us);
    if (action == DHCP_HOLD) { return; }

    in_renew = 1;
    if (action == DHCP_RENEW)        { dhcp_renew(0); }
    else if (action == DHCP_REBIND)  { dhcp_renew(1); }
    else /* DHCP_REACQUIRE */        { lease_valid = 0; net_dhcp(); }
    in_renew = 0;
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
    dhcp_renew_check();          // maintain the DHCP lease whenever the net is in use
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

    uint64_t start = timer_now_us();
    while (timer_now_us() - start < NET_TIMEOUT_US) {
        net_pump();
        if (arp_cache_get(ip, mac)) { return 0; }
        if (net_signal_pending()) { return -1; }    // EINTR
        net_wait(20);
    }
    return -1;                                       // timed out
}
