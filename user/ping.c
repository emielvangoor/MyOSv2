// ping.c -- /bin/ping: resolve a host (via DNS) and ICMP-echo it once a second,
// printing each reply's round-trip time, until interrupted with Ctrl-C. With no
// argument it pings the QEMU gateway (10.0.2.2).
//   ping                     -> ping 10.0.2.2
//   ping example.com         -> resolve, then ping
//   ping https://google.com  -> scheme + path stripped, then resolve + ping
//   ping 1.1.1.1             -> literal IP, no DNS
#include "ulib.h"

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }
static void put_int(int v)
{
    char b[16]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}
static void put_ip(unsigned int ip)
{
    put_int((int)((ip >> 24) & 0xff)); puts1(".");
    put_int((int)((ip >> 16) & 0xff)); puts1(".");
    put_int((int)((ip >> 8)  & 0xff)); puts1(".");
    put_int((int)( ip        & 0xff));
}

// Copy the bare hostname out of `url` into dst: drop a leading "scheme://" and
// anything from the first '/' (a path). E.g. "https://www.google.com/x" -> the
// host "www.google.com".
static void hostname_of(const char *url, char *dst, int cap)
{
    const char *p = url;
    for (const char *s = url; s[0]; s++) {
        if (s[0] == ':' && s[1] == '/' && s[2] == '/') { p = s + 3; break; }
    }
    int i = 0;
    while (p[i] && p[i] != '/' && i < cap - 1) { dst[i] = p[i]; i++; }
    dst[i] = 0;
}

// Parse a dotted-quad "a.b.c.d" into a host-order IP. Returns 0 (and leaves *ip
// untouched) if the string isn't a well-formed IPv4 literal, so the caller can
// fall back to DNS.
static int parse_ipv4(const char *s, unsigned int *ip)
{
    unsigned int v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') { return 0; }      // need at least one digit
        unsigned int octet = 0;
        while (*s >= '0' && *s <= '9') { octet = octet * 10 + (unsigned)(*s - '0'); s++; }
        if (octet > 255) { return 0; }
        v = (v << 8) | octet;
        if (part < 3) { if (*s != '.') { return 0; } s++; }
    }
    if (*s != 0) { return 0; }                        // trailing junk
    *ip = v;
    return 1;
}

int umain(int argc, char **argv)
{
    unsigned int ip;
    char host[128];

    if (argc >= 2) {
        hostname_of(argv[1], host, sizeof(host));
        if (parse_ipv4(host, &ip)) {                  // a literal IP: no DNS needed
            puts1("PING "); put_ip(ip); puts1(":\n");
        } else {
            ip = resolve(host);
            if (ip == 0) { puts1("ping: cannot resolve "); puts1(host); puts1("\n"); return 1; }
            puts1("PING "); puts1(host); puts1(" ("); put_ip(ip); puts1("):\n");
        }
    } else {
        ip = 0x0a000202u;                       // 10.0.2.2 (gateway)
        puts1("PING "); put_ip(ip); puts1(":\n");
    }

    // Echo once a second forever, one line per round trip, until Ctrl-C (SIGINT)
    // terminates us. Timeouts are reported but don't stop the loop -- just like
    // a real ping, a host that's briefly unreachable keeps being probed.
    for (int seq = 0; ; seq++) {
        int ms = -1;
        if (ping(ip, &ms) == 0) {
            puts1("reply from "); put_ip(ip);
            puts1(": seq="); put_int(seq);
            puts1(" time="); put_int(ms); puts1(" ms\n");
        } else {
            puts1("request to "); put_ip(ip);
            puts1(" timed out: seq="); put_int(seq); puts1("\n");
        }
        sys_sleep(1000);                        // ~1 s between probes
    }
    return 0;                                   // unreached (Ctrl-C exits)
}
