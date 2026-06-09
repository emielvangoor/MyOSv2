// poll.c -- the non-blocking readiness scan behind SYS_POLL. See poll.h.
#include "poll.h"
#include "vfs.h"
#include "pipe.h"
#include "socket.h"

int poll_scan(struct file **fds, struct pollfd *pf, int nfds, int nfd_slots)
{
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        pf[i].revents = 0;
        int fd = pf[i].fd;
        if (fd < 0) { continue; }                       // a negative fd is skipped
        if (fd >= nfd_slots || !fds[fd]) {              // closed/invalid descriptor
            pf[i].revents = POLLERR;
            ready++;
            continue;
        }
        struct file *f = fds[fd];

        int rd = 0, wr = 0, hup = 0;
        if (f->sock) {
            rd = socket_readable(f->sock);
            wr = socket_writable(f->sock);
        } else if (f->pipe) {
            rd = pipe_readable(f);
            wr = pipe_writable(f);
            hup = pipe_hangup(f);
        } else {
            // A regular file (or the console placeholder): treat as always ready;
            // ordinary file reads/writes don't block here.
            rd = 1; wr = 1;
        }

        short re = 0;
        if ((pf[i].events & POLLIN)  && rd)  { re |= POLLIN; }
        if ((pf[i].events & POLLOUT) && wr)  { re |= POLLOUT; }
        if (hup)                             { re |= POLLHUP; }   // always reported
        pf[i].revents = re;
        if (re) { ready++; }
    }
    return ready;
}
