// ulib.h -- the tiny user-space C library (syscall wrappers).
#pragma once
long sys_write(int fd, const void *buf, long len);
long sys_read(int fd, void *buf, long len);
long sys_open(const char *path);
long sys_creat(const char *path);  // open, creating the file if missing
long sys_close(int fd);
void sys_exit(int code);
long sys_getpid(void);
void sys_sleep(long ms);
long sys_fork(void);
long sys_exec(const char *path, char *const argv[]);
long sys_wait(int *status);
long sys_readdir(const char *path, int index, char *name);
int  sys_getc(void);
void *sys_sbrk(long incr);
void *malloc(unsigned long n);
void  free(void *p);
void *mmap(unsigned long len);
int   munmap(void *addr, unsigned long len);
int   shm_create(unsigned long len);
void *shm_map(int handle);
int   pipe(int fd[2]);
int   dup2(int oldfd, int newfd);
int   kill(int pid, int sig);
int   signal(int sig, void (*handler)(int));
void  __sigreturn(void);
int   ping(unsigned int ip, int *ms);   // ip in host order; 0 + round-trip, -1 on timeout
unsigned int resolve(const char *host); // hostname -> IP (host order), 0 on failure
void shutdown(void);                    // halt the machine (does not return)

// UDP sockets. ip/port are host order. socket(SOCK_DGRAM) -> fd.
#define SOCK_DGRAM  1
#define SOCK_STREAM 2
int socket(int type);
int bind(int fd, unsigned short port);
int sendto(int fd, const void *buf, int len, unsigned int ip, unsigned short port);
int recvfrom(int fd, void *buf, int len, unsigned int *ip, unsigned short *port);
int connect(int fd, unsigned int ip, unsigned short port);   // TCP connect (SOCK_STREAM)
int listen(int fd, int backlog);                             // TCP passive open
int accept(int fd);                                          // block for a connection -> new fd
int sock_shutdown(int fd, int how);                          // TCP half-close (SHUT_WR/...)
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

// poll(): wait until one of several fds is ready (or a timeout in ms; -1 = forever,
// 0 = return immediately). Returns the number of ready fds, 0 on timeout, -1 EINTR.
#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
struct pollfd { int fd; short events; short revents; };
int poll(struct pollfd *fds, int nfds, int timeout_ms);

// Input events (virtio keyboard + tablet): the Linux-evdev triple, end to end.
// type: 1=EV_KEY (code=KEY_*/BTN_*, value 1=down 0=up), 3=EV_ABS (code 0=X 1=Y,
// value 0..32767), 0=EV_SYN (event-batch separator).
struct input_event { unsigned short type, code; unsigned int value; };
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define ABS_X  0
#define ABS_Y  1
int input_read(struct input_event *ev);   // blocks; 0 on event, -1 on signal

// The framebuffer (virtio-gpu scanout): gfx_acquire maps the 1280x720 BGRX
// framebuffer into this process; write 0x00RRGGBB words, then gfx_flush the
// changed rect to make it visible.
struct gfx_info { void *fb; unsigned int w, h, pitch; };
int gfx_acquire(struct gfx_info *gi);
int gfx_flush(int x, int y, int w, int h);

#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15
long ustrlen(const char *s);
// Each program defines its own umain(); crt0 calls it with x0=argc, x1=argv, so
// a program may declare it as umain(void) or umain(int argc, char **argv).
// (No prototype here on purpose -- it would force one signature on all programs.)
