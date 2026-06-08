// socket.c -- UDP datagram sockets.
// =================================
//
// Sockets sit between user programs (via the fd table + socket syscalls) and the
// UDP layer in netstack. Each holds a local port and a FIFO of received
// datagrams. Delivery is interrupt-driven: the NIC IRQ wakes the network
// wait-channel, recvfrom() pumps the stack (which routes datagrams here via
// socket_udp_input), then dequeues -- no busy polling.

#include <stdint.h>
#include "socket.h"
#include "kheap.h"
#include "net.h"
#include "sched.h"

#define NSOCK     8       // simultaneous sockets
#define DGRAM_MAX 1500    // largest datagram we buffer
#define QDEPTH    8       // datagrams queued per socket before we drop

// A received datagram, kmalloc'd on arrival and freed when recvfrom consumes it.
struct dgram {
    uint32_t      sip;
    uint16_t      sport;
    int           len;
    struct dgram *next;
    uint8_t       data[DGRAM_MAX];
};

struct socket {
    int           used;
    int           type;
    uint16_t      lport;        // local (bound) port; 0 = unbound
    struct dgram *qhead, *qtail;
    int           qcount;
};

static struct socket socks[NSOCK];
static uint16_t      next_eph = 49152;   // ephemeral port cursor

struct socket *socket_alloc(int type)
{
    for (int i = 0; i < NSOCK; i++) {
        if (!socks[i].used) {
            socks[i].used = 1; socks[i].type = type; socks[i].lport = 0;
            socks[i].qhead = socks[i].qtail = 0; socks[i].qcount = 0;
            return &socks[i];
        }
    }
    return 0;
}

void socket_free(struct socket *s)
{
    if (!s) { return; }
    for (struct dgram *d = s->qhead; d; ) { struct dgram *n = d->next; kfree(d); d = n; }
    s->used = 0; s->qhead = s->qtail = 0; s->qcount = 0; s->lport = 0;
}

int socket_bind(struct socket *s, uint16_t port)
{
    if (!s) { return -1; }
    for (int i = 0; i < NSOCK; i++) {            // port must be free
        if (socks[i].used && &socks[i] != s && socks[i].lport == port) { return -1; }
    }
    s->lport = port;
    return 0;
}

int socket_sendto(struct socket *s, const void *buf, int len,
                  uint32_t dst_ip, uint16_t dst_port)
{
    if (!s || s->type != SOCK_DGRAM) { return -1; }
    if (s->lport == 0) {                          // auto-bind an ephemeral port
        s->lport = next_eph++;
        if (next_eph == 0) { next_eph = 49152; }
    }
    return net_udp_send(dst_ip, s->lport, dst_port, buf, len);
}

void socket_udp_input(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                      const uint8_t *data, int len)
{
    for (int i = 0; i < NSOCK; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->type != SOCK_DGRAM || s->lport != dst_port) { continue; }
        if (s->qcount >= QDEPTH) { return; }      // queue full -> drop
        struct dgram *d = kmalloc(sizeof(struct dgram));
        if (!d) { return; }
        d->sip = src_ip; d->sport = src_port;
        if (len > DGRAM_MAX) { len = DGRAM_MAX; }
        d->len = len;
        for (int k = 0; k < len; k++) { d->data[k] = data[k]; }
        d->next = 0;
        if (s->qtail) { s->qtail->next = d; } else { s->qhead = d; }
        s->qtail = d; s->qcount++;
        return;
    }
}

int socket_recvfrom(struct socket *s, void *buf, int len,
                    uint32_t *src_ip, uint16_t *src_port)
{
    if (!s || s->type != SOCK_DGRAM) { return -1; }

    // Pump the network until a datagram is queued for us, sleeping on the NIC
    // interrupt between tries. A pending signal aborts (EINTR).
    while (s->qcount == 0) {
        net_pump();
        if (s->qcount > 0) { break; }
        struct thread *t = sched_current();
        if (t && t->sig_pending) { return -1; }   // EINTR
        net_wait(20);
    }

    struct dgram *d = s->qhead;
    s->qhead = d->next;
    if (!s->qhead) { s->qtail = 0; }
    s->qcount--;

    int n = d->len; if (n > len) { n = len; }
    uint8_t *out = buf;
    for (int k = 0; k < n; k++) { out[k] = d->data[k]; }
    if (src_ip)   { *src_ip = d->sip; }
    if (src_port) { *src_port = d->sport; }
    kfree(d);
    return n;
}
