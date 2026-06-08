# ARM64 Learning OS — Phase 3 Design (Virtual Memory / MMU)

**Date:** 2026-06-08
**Status:** Approved

## Goal

Turn on the ARM64 MMU so every address the kernel uses becomes a **virtual**
address translated through page tables we control. Bootstrap safely with an
**identity map** (virtual == physical) so enabling translation changes nothing
visible, then add one **non-identity** mapping and demonstrate that virtual and
physical addresses can point at the same memory.

Stays at EL1, building on Phase 0–2 (UART, exceptions, GIC, timer).

## Approach & fixed decisions

| Decision | Choice | Why |
|----------|--------|-----|
| Granule | 4 KiB | The standard; matches `TG0=0` |
| Virtual address size | 48-bit (`T0SZ=16`) | Plenty of room; enables a high demo VA |
| Mapping size | 2 MiB blocks | Real L0→L1→L2 walk without thousands of 4 KiB entries |
| Bootstrap | Identity map | Enabling the MMU must not move the PC/SP out from under us |
| Caches | Enabled with the MMU | Normal memory attributes exist to allow caching |
| Translation base | `TTBR0_EL1` only | Low VAs; `TTBR1_EL1` walks disabled (`EPD1=1`) |

### QEMU `virt` memory map (the facts this relies on)

- `0x00000000`–`0x40000000`: device/MMIO (GIC `0x08000000`, UART `0x09000000`).
- `0x40000000`+: RAM. Kernel loads at `0x40080000`; it is tiny (well under 1 MiB).
- `0x40200000`: free RAM, used as the demo's physical target.

## Page-table structure

Five 4 KiB-aligned `uint64_t[512]` tables in BSS:

```
TTBR0_EL1 -> L0[0] -> L1 +- L1[0] -> L2_low   : VA 0x00000000-0x40000000  Device,  identity
                         +- L1[1] -> L2_high  : VA 0x40000000-0x80000000  Normal,  identity
                         +- L1[4] -> L2_demo  : VA 0x100000000 -> PA 0x40200000  Normal (demo)
```

- `L2_low`: 512 × 2 MiB blocks, identity, **Device** memory (AttrIndx 0).
- `L2_high`: 512 × 2 MiB blocks, identity, **Normal** cacheable (AttrIndx 1).
- `L2_demo`: entry [0] = 2 MiB block, VA `0x1_0000_0000` → PA `0x40200000`, Normal.

### Descriptor format (stage 1, 4 KiB granule)

- Table descriptor (L0, L1 → next table): `next_table_pa | 0b11`.
- Block descriptor (L2 2 MiB block): `block_pa | attrs | 0b01`.
- Attribute bits used:
  - `AF` (bit 10) — access flag, **must be 1** or an access-flag fault occurs.
  - `SH` (bits 9:8) — `0b11` inner-shareable for Normal; `0b00` for Device.
  - `AP` (bits 7:6) — `0b00` = read/write at EL1, no EL0 access.
  - `AttrIndx` (bits 4:2) — index into `MAIR_EL1` (0 = Device, 1 = Normal).
  - `PXN`/`UXN` (bits 53/54) — set on Device blocks (non-executable); clear on
    Normal blocks (kernel code is executable).

### Index extraction (4 KiB granule, 48-bit VA)

`L0 = (va>>39)&511`, `L1 = (va>>30)&511`, `L2 = (va>>21)&511`, block offset = low 21 bits.

Demo VA `0x100000000`: L0=0, L1=4, L2=0 → `L1[4]`→`L2_demo`, `L2_demo[0]`→`0x40200000`.

## Control registers

- `MAIR_EL1 = 0xFF00`: Attr0 = `0x00` (Device-nGnRnE), Attr1 = `0xFF` (Normal,
  inner+outer write-back, read/write-allocate).
- `TCR_EL1`: `T0SZ=16`, `IRGN0=0b01`, `ORGN0=0b01`, `SH0=0b11`, `TG0=0b00`
  (4 KiB), `EPD1=1` (disable TTBR1 walks), `IPS=0b010` (40-bit PA).
- `TTBR0_EL1` = physical address of `L0`.
- `SCTLR_EL1`: set bit 0 (`M`, MMU), bit 2 (`C`, data cache), bit 12 (`I`,
  instruction cache).

### Enable sequence (barrier-sensitive)

```
dsb ish                         // table writes complete & visible
msr mair_el1 / tcr_el1 / ttbr0_el1
isb
tlbi vmalle1                    // flush stale TLB entries
dsb ish
isb
read sctlr_el1; set M|C|I; write back
isb                            // MMU now ON
```

## Translation demo

After `mmu_init()` returns (MMU on), `kmain`:
1. Writes `0xDEADBEEF` to virtual address `0x100000000`.
2. Reads physical address `0x40200000` (identity-mapped, reachable at its own address).
3. Prints both and reports a match.

Two different addresses resolving to the same physical bytes proves real translation.

## New components

| File | Responsibility |
|------|----------------|
| `src/mmu.h` | `mmu_init()` declaration + descriptor/attribute constants |
| `src/mmu.c` | Build the five tables; configure MAIR/TCR/TTBR0; enable the MMU |
| `src/kmain.c` | (modified) call `mmu_init()` before the demo; run the translation demo |

## Success criteria

- Kernel survives MMU enable: banner prints and **timer ticks continue** after
  `mmu_init()` (identity map + interrupts + caches all working together).
- Demo prints e.g.
  `wrote 0xDEADBEEF via VA 0x100000000, read 0xDEADBEEF via PA 0x40200000 -- match!`.
- `SCTLR_EL1` read back shows bit 0 (`M`) set.

## Out of scope (later phases)

Per-page (4 KiB) `map_page()` API, higher-half kernel, per-process address
spaces / `TTBR0` switching (Phase 5+), EL0 user mappings (Phase 6), demand paging.
