// mmu.h -- interface to the memory-management unit (MMU) setup.
// Calling mmu_init() builds page tables and turns on virtual-memory
// translation, after which every address goes through the tables we built.
#pragma once
#include <stdint.h>

void mmu_init(void);

// L0[0] descriptor for the shared kernel mapping (identity 0-2 GiB, EL1-only).
// Per-process address spaces install this so the kernel works after a trap.
uint64_t mmu_kernel_l0_entry(void);
