/* hello.c -- the first REAL Linux binary on MyOSv2: built with
 * aarch64-linux-musl-gcc -static -no-pie, it makes Linux syscalls (write,
 * exit_group, ...) that our migrated ABI now answers. */
#include <stdio.h>

int main(void)
{
    printf("hello from musl on MyOSv2!\n");
    return 0;
}
