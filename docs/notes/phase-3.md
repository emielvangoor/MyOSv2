# Phase 3 notes — Virtual Memory (MMU)

## What changed

The MMU is now on. Every address the kernel uses is a **virtual** address that
the CPU translates through page tables we built. We bootstrapped with an identity
map (so enabling translation changed nothing visible) and added one non-identity
mapping to prove translation is real. Data + instruction caches are on too.

Observed: `SCTLR_EL1 = 0xc5183d` (M=1, C=1, I=1), `TTBR0_EL1 = 0x40086000`
(== `&l0_table`).

## Virtual vs physical, and why identity-mapping is safe

With the MMU off, addresses go straight to hardware. With it on, the CPU walks a
tree of page tables to turn a virtual address into a physical one. The hazard:
the instant `SCTLR_EL1.M` flips, the *next instruction fetch* is translated. If
the PC's address isn't mapped, the CPU faults immediately. Identity-mapping
(VA == PA) for all memory we use means the PC, SP, code, and devices keep
resolving to the same place — so enabling translation is invisible and safe.

## The page-table walk (4 KiB granule, 48-bit VA)

Four levels (L0→L1→L2→L3); a 2 MiB block stops the walk at L2. Index bit-fields
of a virtual address:

```
 47        39 38        30 29        21 20        12 11         0
+------------+------------+------------+------------+------------+
|  L0 index  |  L1 index  |  L2 index  |  L3 index  |   offset   |
+------------+------------+------------+------------+------------+
   (va>>39)&511  (>>30)&511  (>>21)&511  (>>12)&511
```

Each L0 entry spans 512 GiB, each L1 entry 1 GiB, each L2 entry 2 MiB.

Our tree:
```
TTBR0_EL1 -> l0_table[0] -> l1_table
                              [0] -> l2_low   : VA 0-1GB    Device, identity
                              [1] -> l2_high  : VA 1-2GB    Normal, identity
                              [4] -> l2_demo  : VA 4-5GB    [0] -> PA 0x40200000
```
Demo VA `0x100000000` = L0 0, L1 4, L2 0 → resolves to PA `0x40200000`.

## Descriptor format (64-bit entries)

- Low 2 bits: `0b11` = table (points to next level), `0b01` = block mapping.
- `AF` (bit 10): access flag. **Must be 1** or the walk faults (access-flag fault).
- `SH` (bits 9:8): shareability. Inner-shareable (`0b11`) for Normal; `0b00` for Device.
- `AP` (bits 7:6): `0b00` = EL1 read/write, no EL0.
- `AttrIndx` (bits 4:2): which `MAIR_EL1` slot describes this memory's type.
- `PXN`/`UXN` (53/54): execute-never. Set on Device blocks; clear on Normal (code).

## MAIR_EL1 — memory types

A table of 8 one-byte type descriptors. We use:
- Slot 0 = `0x00`: Device-nGnRnE (strongly ordered MMIO).
- Slot 1 = `0xFF`: Normal, inner+outer write-back, read/write-allocate (cacheable).
`MAIR_EL1 = 0xFF00`. Descriptors reference these by index via `AttrIndx`.

## TCR_EL1 — translation control (key fields)

- `T0SZ = 16`: TTBR0 covers a 48-bit VA space (2^(64-16)).
- `TG0 = 0`: 4 KiB granule.
- `IRGN0/ORGN0 = 1`, `SH0 = 3`: page-table walks are cacheable + inner-shareable.
- `EPD1 = 1`: disable TTBR1 walks (we only use the low half / TTBR0).
- `IPS = 2`: 40-bit physical address size.

## The enable sequence (and why each barrier)

```
build tables; dsb ish            // make table writes visible to the table walker
msr mair_el1 / tcr_el1 / ttbr0_el1
isb                              // these system-reg writes take effect before...
tlbi vmalle1                     // ...flushing any stale TLB entries for EL1
dsb ish                          // wait for the TLB flush to complete
isb
set SCTLR_EL1 M|C|I; isb         // MMU + caches ON; isb so the next fetch is translated
```
- `dsb` orders memory/maintenance ops; `isb` flushes the pipeline so following
  instructions see the new system state.
- Skipping the `tlbi` risks honoring stale translations; skipping the final `isb`
  risks the next instruction being fetched with the old (off) translation.

## How the demo proves translation

`*(uint32_t*)0x100000000 = 0xDEADBEEF;` writes through the virtual alias, which the
MMU translates to PA `0x40200000`. Reading `*(uint32_t*)0x40200000` (identity, its
own address) returns `0xDEADBEEF`. Two different addresses, one physical cell.
GDB independently shows `0x40200000` holds `0xdeadbeef`.

## Gotcha: RWX LOAD segment warning

The linker warns the single LOAD segment is read/write/execute. Harmless here
(one combined segment for a tiny kernel). Could be cleaned up later by splitting
code/data into separate segments with distinct permissions.
