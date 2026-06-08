// semihost.c -- ARM semihosting: ask QEMU to exit with a status code.
// ===================================================================
//
// "Semihosting" is a debug channel where the guest asks the host (here QEMU) to
// do something on its behalf -- file I/O, console, or EXIT. The guest puts an
// operation number in x0, a parameter (block) pointer in x1, and executes the
// AArch64 semihosting trap `HLT #0xF000`. QEMU (run with -semihosting) handles
// it. We only use SYS_EXIT, to end a test run with a pass/fail code.

#include <stdint.h>
#include "semihost.h"

#define SYS_EXIT 0x18                       // semihosting "exit" operation
#define ADP_Stopped_ApplicationExit 0x20026 // "the application exited normally"

void qemu_exit(int code)
{
    // On AArch64, SYS_EXIT takes a two-word block: { reason, exit_status }.
    // QEMU exits its own process with exit_status as the code.
    uint64_t block[2] = { ADP_Stopped_ApplicationExit,
                          (uint64_t)(unsigned int)code };

    register uint64_t x0 __asm__("x0") = SYS_EXIT;
    register uint64_t x1 __asm__("x1") = (uint64_t)block;
    __asm__ volatile("hlt #0xF000" : : "r"(x0), "r"(x1) : "memory");

    // Only reached if semihosting is disabled (HLT would otherwise fault). We
    // only call this in the -semihosting test build, so just halt defensively.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
