// mmu.h -- interface to the memory-management unit (MMU) setup.
// Calling mmu_init() builds page tables and turns on virtual-memory
// translation, after which every address goes through the tables we built.
#pragma once

void mmu_init(void);

// The EL0 alias of RAM lives at (physical address + this offset). User threads
// run at EL0 from this window (VA 0x80000000..0xC0000000 -> PA 0x40000000..),
// which is mapped EL0-accessible while the kernel's identity mapping stays
// EL1-only. To get the user-visible address of a kernel object: addr + offset.
#define USER_ALIAS_OFFSET 0x40000000UL
