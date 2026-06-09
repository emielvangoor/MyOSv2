// poll.h -- readiness multiplexing over several file descriptors.
// ===============================================================
//
// A blocking read/recv only waits on ONE fd. A program that must watch several
// at once -- a server juggling many connections, or anything mixing a socket and
// a pipe -- needs to ask "which of these are ready?" without committing to a
// blocking read on any single one. That is poll(): hand it an array of fds and
// the events you care about, get back which ones can proceed.
//
// `poll_scan` is the pure, non-blocking heart: one pass that fills in `revents`
// from each fd's current readiness. The SYS_POLL handler wraps it in the usual
// pump-and-sleep loop until at least one fd is ready or the timeout elapses.
#pragma once

struct file;

// Poll event bits (a small, POSIX-flavoured subset).
#define POLLIN  0x001   // readable now (data queued, or EOF/peer-closed)
#define POLLOUT 0x004   // writable now (send/write won't block on a closed window)
#define POLLERR 0x008   // error condition (also reported for a bad fd)
#define POLLHUP 0x010   // peer hung up (the readable side reached EOF)

struct pollfd {
    int   fd;           // the descriptor to watch (negative => ignored)
    short events;       // requested events (POLLIN/POLLOUT)
    short revents;      // returned events (filled by poll)
};

// One non-blocking pass: for each pollfd, look up fds[fd] and set revents from
// its current readiness intersected with the requested events. Returns the count
// of pollfds that ended up with a non-zero revents.
int poll_scan(struct file **fds, struct pollfd *pf, int nfds, int nfd_slots);
