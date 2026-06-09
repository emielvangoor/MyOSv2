// httpd.c -- a tiny HTTP/1.0 server, the passive-open counterpart to http.c.
// ==========================================================================
//
// Demonstrates the Phase-23.4 server path: socket -> bind -> listen -> accept,
// then read the request and write a canned response. Serves connections one at a
// time, forever, until interrupted (Ctrl-C makes accept() fail and we exit).
//
//   httpd        -> listen on :8080
//   httpd 9090   -> listen on :9090
//
// From the host (QEMU forwards host:8080 -> guest:8080, see the Makefile):
//   curl http://localhost:8080/
#include "ulib.h"

static void puts1(const char *s) { sys_write(1, s, ustrlen(s)); }

// Print a small non-negative integer (no trailing newline).
static void put_int(long v)
{
    char b[16];
    int i = 0;
    if (v == 0) { b[i++] = '0'; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { sys_write(1, &b[--i], 1); }
}

// Parse a positive decimal port from a string; 0 if empty/invalid.
static unsigned short parse_port(const char *s)
{
    unsigned v = 0;
    if (!s || !*s) { return 0; }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { return 0; }
        v = v * 10 + (unsigned)(*s - '0');
    }
    return (unsigned short)v;
}

int umain(int argc, char **argv)
{
    unsigned short port = argc >= 2 ? parse_port(argv[1]) : 0;
    if (port == 0) { port = 8080; }

    int sfd = socket(SOCK_STREAM);
    if (sfd < 0) { puts1("httpd: socket failed\n"); return 1; }
    if (bind(sfd, port) != 0)   { puts1("httpd: bind failed\n");   sys_close(sfd); return 1; }
    if (listen(sfd, 4) != 0)    { puts1("httpd: listen failed\n"); sys_close(sfd); return 1; }

    puts1("httpd: listening on :"); put_int(port); puts1(" (Ctrl-C to stop)\n");

    // A body deliberately larger than one MSS (1400 B), so the response is sent as
    // several TCP segments -- exercising the segmentation/pipelining path. Each
    // line is 50 chars incl. '\n'; 80 lines = 4000 bytes.
    static char body[4000];
    const int BODY = (int)sizeof(body);
    for (int i = 0; i < BODY; i++) {
        int col = i % 50;
        body[i] = (col == 49) ? '\n' : (char)('a' + (i / 50) % 26);
    }
    const char *hdr =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 4000\r\n"
        "Connection: close\r\n"
        "\r\n";

    int served = 0;
    for (;;) {
        int cfd = accept(sfd);                 // blocks until a client connects
        if (cfd < 0) { puts1("httpd: stopped\n"); break; }   // interrupted

        char buf[512];
        int n = (int)sys_read(cfd, buf, sizeof(buf));   // read the request line(s)
        (void)n;
        sys_write(cfd, hdr, ustrlen(hdr));
        sys_write(cfd, body, BODY);            // 4000 bytes -> multiple segments
        sys_close(cfd);

        served++;
        puts1("httpd: served request #"); put_int(served); puts1("\n");
    }

    sys_close(sfd);
    return 0;
}
