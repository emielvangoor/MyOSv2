// elf.h -- minimal ELF64 program loader interface.
// ================================================
//
// An ELF executable is a header describing where its pieces ("segments") want to
// live in virtual memory. We read the header + the program-header table and map
// each PT_LOAD segment at its requested address. We only support what our own
// freestanding programs emit: ELF64, little-endian, AArch64, no dynamic linking.
#pragma once
#include <stdint.h>

struct addrspace;   // from vm.h

// ELF64 file header (we use a subset of the fields).
typedef struct {
    uint8_t  e_ident[16];   // magic + class/data/version
    uint16_t e_type;
    uint16_t e_machine;     // 0xB7 = EM_AARCH64
    uint32_t e_version;
    uint64_t e_entry;       // entry point virtual address
    uint64_t e_phoff;       // program-header table file offset
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;   // size of one program-header entry
    uint16_t e_phnum;       // number of program-header entries
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

// ELF64 program header (one per segment).
typedef struct {
    uint32_t p_type;        // 1 = PT_LOAD
    uint32_t p_flags;       // PF_X=1, PF_W=2, PF_R=4
    uint64_t p_offset;      // file offset of the segment's bytes
    uint64_t p_vaddr;       // virtual address to map it at
    uint64_t p_paddr;
    uint64_t p_filesz;      // bytes present in the file
    uint64_t p_memsz;       // bytes in memory (>= filesz; the extra is .bss)
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2
#define PF_R    4

// Map every PT_LOAD segment of the ELF image into `as`, returning the entry
// point in *entry. Returns 0 on success, -1 if the image isn't a valid ELF64
// AArch64 executable.
int elf_load(struct addrspace *as, const void *img, uint64_t len, uint64_t *entry);
