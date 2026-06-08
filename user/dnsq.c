// dnsq.c -- /bin/dnsq: a DNS query done entirely from user space over the socket
// API, to prove socket()/sendto()/recvfrom() work end-to-end. Sends an A-record
// query for the given host (default example.com) to the DNS server and prints
// the address from the reply.
//   dnsq            -> query example.com
//   dnsq github.com -> query github.com
#include "ulib.h"

#define IP_DNS 0x0a000203u   // 10.0.2.3 (QEMU's resolver)

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }
static void put_int(int v)
{
    char b[16]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

// Encode "a.b.c" as DNS labels [1]a[1]b[1]c[0]; returns the length written.
static int dns_name(unsigned char *out, const char *host)
{
    int w = 0;
    const char *p = host;
    while (*p) {
        const char *s = p;
        while (*p && *p != '.') { p++; }
        int l = (int)(p - s);
        out[w++] = (unsigned char)l;
        for (int i = 0; i < l; i++) { out[w++] = (unsigned char)s[i]; }
        if (*p == '.') { p++; }
    }
    out[w++] = 0;
    return w;
}

int umain(int argc, char **argv)
{
    const char *host = (argc >= 2) ? argv[1] : "example.com";

    int fd = socket(SOCK_DGRAM);
    if (fd < 0) { puts1("dnsq: socket failed\n"); return 1; }

    unsigned char q[256];
    q[0] = 0x12; q[1] = 0x34;          // transaction id
    q[2] = 0x01; q[3] = 0x00;          // flags: recursion desired
    q[4] = 0; q[5] = 1;                // 1 question
    q[6] = 0; q[7] = 0; q[8] = 0; q[9] = 0; q[10] = 0; q[11] = 0;
    int n = 12;
    n += dns_name(q + 12, host);
    q[n++] = 0; q[n++] = 1;            // QTYPE = A
    q[n++] = 0; q[n++] = 1;            // QCLASS = IN

    if (sendto(fd, q, n, IP_DNS, 53) < 0) { puts1("dnsq: sendto failed\n"); sys_close(fd); return 1; }

    unsigned char r[512];
    unsigned int sip = 0; unsigned short sport = 0;
    int m = recvfrom(fd, r, sizeof(r), &sip, &sport);
    sys_close(fd);
    if (m < 4) { puts1("dnsq: no reply\n"); return 1; }

    // For a simple single-A response the address is the reply's last 4 bytes.
    puts1("dns: "); puts1(host); puts1(" -> ");
    put_int(r[m-4]); puts1("."); put_int(r[m-3]); puts1(".");
    put_int(r[m-2]); puts1("."); put_int(r[m-1]);
    puts1("  ("); put_int(m); puts1(" bytes via socket)\n");
    return 0;
}
