// http.c -- /bin/http: fetch a URL over TCP and print the response. Proves the
// TCP client (socket(SOCK_STREAM) + connect + write + read + close) end to end.
//   http              -> GET example.com
//   http 1.1.1.1      -> GET that IP's "/"
// (Hostnames are resolved with the DNS resolver; only "/" is requested.)
#include "ulib.h"

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }
static void put_int(int v)
{
    char b[16]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

// Strip a leading "scheme://" and anything from the first '/'.
static void host_of(const char *url, char *dst, int cap)
{
    const char *p = url;
    for (const char *s = url; s[0]; s++) {
        if (s[0] == ':' && s[1] == '/' && s[2] == '/') { p = s + 3; break; }
    }
    int i = 0;
    while (p[i] && p[i] != '/' && i < cap - 1) { dst[i] = p[i]; i++; }
    dst[i] = 0;
}

static int parse_ipv4(const char *s, unsigned int *ip)
{
    unsigned int v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') { return 0; }
        unsigned int o = 0;
        while (*s >= '0' && *s <= '9') { o = o * 10 + (unsigned)(*s - '0'); s++; }
        if (o > 255) { return 0; }
        v = (v << 8) | o;
        if (part < 3) { if (*s != '.') { return 0; } s++; }
    }
    if (*s != 0) { return 0; }
    *ip = v; return 1;
}

// Build "GET / HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n".
static int build_request(char *out, const char *host)
{
    const char *a = "GET / HTTP/1.0\r\nHost: ";
    const char *b = "\r\nConnection: close\r\n\r\n";
    int n = 0;
    for (const char *p = a; *p; p++) { out[n++] = *p; }
    for (const char *p = host; *p; p++) { out[n++] = *p; }
    for (const char *p = b; *p; p++) { out[n++] = *p; }
    return n;
}

int umain(int argc, char **argv)
{
    char host[128];
    host_of(argc >= 2 ? argv[1] : "example.com", host, sizeof(host));

    unsigned int ip;
    if (!parse_ipv4(host, &ip)) {
        ip = resolve(host);
        if (ip == 0) { puts1("http: cannot resolve "); puts1(host); puts1("\n"); return 1; }
    }

    int fd = socket(SOCK_STREAM);
    if (fd < 0) { puts1("http: socket failed\n"); return 1; }

    puts1("connecting to "); puts1(host); puts1(":80 ...\n");
    if (connect(fd, ip, 80) != 0) { puts1("http: connect failed\n"); sys_close(fd); return 1; }

    char req[256];
    int rn = build_request(req, host);
    if (sys_write(fd, req, rn) < 0) { puts1("http: write failed\n"); sys_close(fd); return 1; }

    // Read and print the response until the peer closes (read returns 0).
    char buf[512];
    int total = 0, n;
    while ((n = (int)sys_read(fd, buf, sizeof(buf))) > 0) {
        sys_write(1, buf, n);
        total += n;
    }
    sys_close(fd);
    puts1("\n--- received "); put_int(total); puts1(" bytes ---\n");
    return 0;
}
