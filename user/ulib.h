// ulib.h -- the tiny user-space C library (syscall wrappers).
#pragma once
long sys_write(int fd, const void *buf, long len);
long sys_read(int fd, void *buf, long len);
long sys_open(const char *path);
long sys_close(int fd);
void sys_exit(int code);
long sys_getpid(void);
void sys_sleep(long ms);
long ustrlen(const char *s);
void umain(void);   // the program's entry (defined per-program)
