# Phase 0 + 1 notes

## Toolchain versions

Installed via Homebrew on Apple Silicon macOS (2026-06-07):

- `aarch64-elf-gcc` (GCC) 16.1.0 — cross-compiler producing freestanding AArch64 ELF
- `aarch64-elf-ld` (GNU Binutils) 2.46.0 — linker + objcopy/objdump for inspection
- `aarch64-elf-gdb` (GDB) 17.2 — source-level debugger speaking ARM64
- `qemu-system-aarch64` (QEMU) 11.0.0 — emulates the `virt` ARM64 machine

### Why a cross-compiler?

Apple's clang emits Mach-O executables that assume macOS underneath. A bare-metal
kernel has no OS underneath and must be an ELF image with no C runtime. The
`aarch64-elf-*` toolchain targets exactly that: AArch64, ELF, freestanding.
