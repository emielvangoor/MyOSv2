// vm.h -- per-process address spaces (each user process has its own page tables).
#pragma once
#include <stdint.h>

// User virtual addresses (all in one 2 MiB region under a private L0 entry, so a
// single L3 table covers code + stack + data).
#define USER_CODE_VA   0x8000000000UL   // shared, read-only, EL0-executable
#define USER_STACK_TOP 0x8000100000UL   // private stack grows down from here
#define USER_DATA_VA   0x8000180000UL   // one private data page

struct addrspace {
    uint64_t *l0;   // top-level table (a PMM page); load into TTBR0_EL1
};

void vm_init(void);                              // one-time: prepare shared code pages
struct addrspace *as_create(void);               // kernel-shared + private user maps
uint64_t as_translate(struct addrspace *as, uint64_t va); // software walk -> PA (0 if unmapped)
void as_switch(struct addrspace *as);            // TTBR0 = as->l0; flush TLB
uint64_t user_entry_va(void);                    // USER_CODE_VA + (user_main - __user_start)
