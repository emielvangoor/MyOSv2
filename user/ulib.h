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

#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15
long ustrlen(const char *s);
int  umain(void);   // the program's entry (defined per-program); returns exit status
