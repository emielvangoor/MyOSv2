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

## The boot flow

```
QEMU loads kernel.elf at 0x40080000 and starts ALL cpu cores at _start
  └─ boot.S (_start):
       read MPIDR_EL1 -> core id; park every core except core 0 (wfe loop)
       core 0: set sp = stack_top
               zero the .bss region (__bss_start .. __bss_end)
               bl kmain        ← jump into C
  └─ kmain() (C):
       uart_init(); kprintf(...)  ← output over PL011 serial
       infinite wfe loop
```

## Compiler / linker flags (and why)

- `-ffreestanding` — no hosted-environment assumptions (no libc, no `main` magic).
- `-nostdlib -nostartfiles` — we supply our own `_start`; don't link the C runtime.
- `-mgeneral-regs-only` — don't emit FP/SIMD instructions; we haven't enabled
  that hardware yet, so touching it would fault.
- `-g` — DWARF debug info so GDB can step C/asm by source line.
- `-O2 -ffunction-sections` + `-Wl,--gc-sections` — optimize, then drop unused
  sections so the image stays small.
- `-T linker.ld` — use our memory map.

## Memory layout

- QEMU `virt` RAM starts at `0x40000000`; `-kernel` ELF images are placed at
  `0x40080000`. Our `linker.ld` sets the location counter to `0x40080000` so the
  addresses the linker assigns match where the code actually runs.
- Sections in order: `.text` (`.text.boot` first), `.rodata`, `.data`, `.bss`,
  then a 16 KiB stack ending at `stack_top`.

## PL011 UART (our first device)

- MMIO base on `virt`: `0x09000000`.
- `DR` (offset 0x00) = data register: write a byte to transmit.
- `FR` (offset 0x18) = flag register: bit 5 (TXFF) means the TX FIFO is full.
- Access must be through `volatile` pointers, or the compiler optimizes the
  "useless" memory writes away.

## Exception level at boot

QEMU `virt` starts the kernel at **EL1** (the OS/kernel privilege level).
EL0 = user, EL1 = kernel, EL2 = hypervisor, EL3 = firmware/secure monitor.
Verified at runtime by reading `CurrentEL` (bits [3:2]) — see `current_el()` in
`kmain.c`.

## The debug workflow (two terminals)

- Terminal A: `make debug` — QEMU boots frozen, exposing a GDB stub on `:1234`.
- Terminal B: `make gdb` — `aarch64-elf-gdb` loads `kernel.elf`; the project
  `.gdbinit` auto-connects, breaks at `kmain`, and continues.
- Then: `next`/`step` to walk source lines, `info registers`, `x/...` to read
  memory, `continue` to run.
- One-time setup: `add-auto-load-safe-path .../.gdbinit` was appended to
  `~/.gdbinit` so GDB trusts the project init file.

## Quitting QEMU

`-nographic` routes serial to the terminal; quit with `Ctrl-A` then `X`.

## Surprises / gotchas

- GDB initially reported "no line number information" — fixed by adding `-g`.
  Debug info does not change the running binary; it only adds symbols GDB reads.
