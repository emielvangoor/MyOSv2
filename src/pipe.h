// pipe.h -- an in-kernel unidirectional byte channel (a pipe).
// ===========================================================
//
// A pipe is a fixed ring buffer with two ends: a read end and a write end (each
// a `struct file`). Reading blocks until bytes arrive or all writers close (then
// it returns 0 = EOF); writing blocks until space frees or all readers close
// (then -1 = broken pipe). The `readers`/`writers` counts track how many open
// ends remain, so the pipe is freed only when both reach zero.
#pragma once
#include <stdint.h>

// 16 KiB: large enough that a vterm helper's whole screen-update batch (a full
// 24x80 repaint is a few KiB) fits in the pipe and is written in one atomic
// shot, so the frame never reads a half-written batch. (Was 4096.)
#define PIPE_SIZE 16384

struct file;   // from vfs.h (a pipe end is a file with ->pipe set)

struct pipe {
    char     buf[PIPE_SIZE];
    uint32_t r, w;        // ring read/write positions
    uint32_t count;       // bytes currently buffered
    int      readers;     // number of open read ends
    int      writers;     // number of open write ends
};

struct pipe *pipe_alloc(void);                                  // readers=writers=1
int  pipe_read(struct file *f, void *buf, uint64_t len);        // blocks; 0 = EOF
int  pipe_write(struct file *f, const void *buf, uint64_t len); // blocks; -1 = broken
void pipe_close(struct file *f);                                // drop an end; free when 0/0

// Readiness predicates for poll() (non-blocking; see poll.h).
int  pipe_readable(struct file *f);   // read end has data, or all writers closed (EOF)
int  pipe_writable(struct file *f);   // write end has room, or all readers closed (broken)
int  pipe_hangup(struct file *f);     // read end drained and all writers have closed
