# Phase 6b notes — Per-Process Address Spaces (isolation)

## What changed

Each user process now has its **own page tables** (its own `TTBR0`). Two
processes can use the **same virtual address** and get **different physical
memory** — true isolation. Observed:

```
  [user] process 1 read 1  (ISOLATED)
  [user] process 2 read 2  (ISOLATED)
```

Both wrote their pid to `USER_DATA_VA`, slept (the other ran and wrote its pid to
the same VA in its own page), then read back their **own** pid. Shared memory
would have shown `LEAKED`.

## Address space layout

An `addrspace` is just a top-level table `l0` (a PMM page):

```
TTBR0 -> as->l0 +- l0[0] = SHARED kernel L1 (identity 0-2GB, EL1-only) [mmu_kernel_l0_entry()]
                +- l0[1] -> PRIVATE user L1 -> L2 -> L3 (4 KiB pages):
                             USER_CODE_VA  -> shared code (RO, EL0-exec)
                             USER_STACK..  -> private stack (EL0 RW)
                             USER_DATA_VA  -> private data  (EL0 RW)
```

- **`l0[0]` is shared** in every process: the kernel's identity map (EL1-only).
  This is why the kernel keeps working after a trap regardless of which process
  is active — the kernel is mapped in every address space.
- **`l0[1]` is private**: user code/stack/data, mapped with **4 KiB pages** (an L3
  table), since page-table pages come from the PMM (4 KiB-aligned). User VAs sit
  at >= 512 GiB (`0x80_0000_0000`), a separate L0 entry from the kernel's.
- **Code is shared, stack/data are private.** The code pages hold one copy of the
  user program (same physical in every AS, read-only). Stack and data pages are
  freshly PMM-allocated per process — that's where isolation lives.

## Relocating the user program (a tiny loader)

User code must run at `USER_CODE_VA`, not its link-time kernel address. So:
- The user program lives in a contiguous **`.user` linker section**
  (`__user_start`/`__user_end`), and is **string-literal free** (no `.rodata`), so
  it uses only PC-relative addressing within the blob.
- `vm_init()` copies the `.user` blob into shared code pages once. `as_create()`
  maps those pages at `USER_CODE_VA` in every process. The entry point is
  `USER_CODE_VA + (user_main - __user_start)` (`user_entry_va()`).

## Switching address spaces

`as_switch(as)` writes `TTBR0_EL1 = as->l0`, then `isb; tlbi vmalle1; dsb; isb`
(full TLB flush — no ASIDs yet). `schedule()` calls it whenever it switches to a
thread that has an address space (`best->as != NULL`). Kernel threads have
`as == NULL` and keep whatever tables are active (the kernel map is in all of
them, so it doesn't matter).

## Testing

Actual EL0 isolation is observed in the demo, but the **mapping** is unit-tested
via a software page-table walk `as_translate(as, va)`:
- user data VA -> different physical in two ASs (private);
- user code VA -> same physical (shared);
- a kernel VA -> itself (identity; shared kernel map present);
- an unmapped VA -> 0;
- user stack VA -> different physical in two ASs (private).

Five tests, written test-first.

## A subtlety worth remembering

`as_translate` and `map_page` dereference physical addresses as kernel pointers.
That works only because the kernel **identity-maps** RAM, so a physical table
address equals its kernel virtual address. (A higher-half kernel would need a
phys->virt offset here.)

## Known limits (future work)

Full TLB flush on every switch (no ASIDs); no `fork`/`exec`/copy-on-write; no
demand paging; no shared memory between processes; a flat `.user` blob instead of
ELF parsing; kernel memory is shared read/write across the (single) kernel — fine
for one CPU.
