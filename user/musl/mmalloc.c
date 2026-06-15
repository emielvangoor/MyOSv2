/* mmalloc.c -- exercises musl malloc (brk for small, mmap for large). */
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    char *p = malloc(200000);            /* big -> musl uses mmap */
    if (!p) { printf("malloc failed\n"); return 1; }
    for (int i = 0; i < 200000; i++) { p[i] = (char)(i & 0x7f); }
    long sum = 0;
    for (int i = 0; i < 200000; i++) { sum += p[i]; }
    printf("musl malloc ok, sum=%ld\n", sum);
    free(p);
    return 0;
}
