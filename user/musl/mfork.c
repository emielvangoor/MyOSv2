/* mfork.c -- musl fork + waitpid (clone/wait4 under the hood). */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void)
{
    pid_t p = fork();
    if (p == 0) { printf("child says hi\n"); _exit(3); }
    int st = 0;
    waitpid(p, &st, 0);
    printf("parent reaped child, status=%d\n", WEXITSTATUS(st));
    return 0;
}
