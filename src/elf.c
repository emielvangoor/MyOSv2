// elf.c -- load an ELF64 executable's segments into an address space.
// ===================================================================
//
// We read the file header to find the program-header table, then for each
// PT_LOAD entry we map that segment at its requested virtual address. Mapping +
// .bss zeroing is done by as_map_segment (vm.c); here we just parse and dispatch.

#include <stdint.h>
#include "elf.h"
#include "vm.h"

int elf_load(struct addrspace *as, const void *img, uint64_t len, uint64_t *entry)
{
    if (len < sizeof(Elf64_Ehdr)) {
        return -1;
    }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;

    // Validate: ELF magic, 64-bit class, AArch64 machine.
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        return -1;
    }
    if (eh->e_ident[4] != 2) {        // ELFCLASS64
        return -1;
    }
    if (eh->e_machine != 0xB7) {       // EM_AARCH64
        return -1;
    }

    *entry = eh->e_entry;

    const uint8_t *base = (const uint8_t *)img;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(base + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        as_map_segment(as, ph->p_vaddr, base + ph->p_offset,
                       ph->p_filesz, ph->p_memsz,
                       (ph->p_flags & PF_W) ? 1 : 0,
                       (ph->p_flags & PF_X) ? 1 : 0);
    }
    return 0;
}
