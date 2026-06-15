/* mfile.c -- musl file I/O: open/write/lseek/read/fstat. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
int main(void)
{
    int fd = open("/mfile.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("open failed\n"); return 1; }
    write(fd, "abcdefghij", 10);
    lseek(fd, 0, SEEK_SET);
    char buf[16] = {0};
    int n = read(fd, buf, 10);
    struct stat st;
    fstat(fd, &st);
    printf("read %d bytes: '%s', st_size=%ld\n", n, buf, (long)st.st_size);
    close(fd);
    return 0;
}
