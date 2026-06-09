// ulib.h -- the tiny user-space C library (syscall wrappers).
#pragma once
long sys_write(int fd, const void *buf, long len);
long sys_read(int fd, void *buf, long len);
long sys_open(const char *path);
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

#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15
long ustrlen(const char *s);
// Each program defines its own umain(); crt0 calls it with x0=argc, x1=argv, so
// a program may declare it as umain(void) or umain(int argc, char **argv).
// (No prototype here on purpose -- it would force one signature on all programs.)
