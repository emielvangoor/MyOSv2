// mmu.h -- interface to the memory-management unit (MMU) setup.
// Calling mmu_init() builds page tables and turns on virtual-memory
// translation, after which every address goes through the tables we built.
#pragma once

void mmu_init(void);
