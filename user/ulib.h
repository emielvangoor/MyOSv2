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
long sys_exec(const char *path);
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
long ustrlen(const char *s);
int  umain(void);   // the program's entry (defined per-program); returns exit status
