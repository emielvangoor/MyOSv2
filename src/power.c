// power.c -- turn the machine off via PSCI.
// =========================================
//
// PSCI (Power State Coordination Interface) is the ARM-standard way for an OS to
// ask the firmware/hypervisor to power things off, reset, or bring CPUs up. On
// QEMU's 'virt' board the guest runs at EL1 and PSCI calls are made with the HVC
// ("hypervisor call") instruction, which QEMU intercepts. SYSTEM_OFF powers the
// whole machine down -- so `qemu-system-aarch64` exits cleanly.

#include <stdint.h>
#include "power.h"

#define PSCI_SYSTEM_OFF 0x84000008u   // standard PSCI function id

void power_off(void)
{
    register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_OFF;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    // If PSCI didn't take effect (shouldn't happen on QEMU virt), just halt.
    for (;;) { __asm__ volatile("wfi"); }
}
