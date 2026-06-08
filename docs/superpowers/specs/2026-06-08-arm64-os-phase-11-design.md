# MyOSv2 — Phase 11 Design (ASIDs: tagged TLB entries)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Stop flushing the entire TLB on every context switch. Today `as_switch()` does
`tlbi vmalle1` — it throws away **all** cached translations (kernel + every
process) each time we pick a new thread, so the very next memory access re-walks
the page tables and keeps missing until the TLB refills. That cost is paid on
*every* switch, ~hundreds of times a second.

**ASIDs (Address Space IDentifiers)** fix this. Each address space gets a small
integer ID. The CPU tags every TLB entry it caches with the ID of the address
space that was active, and only *uses* an entry whose tag matches the current
ID. So entries from process A simply sit dormant while process B runs — no flush
needed. Switching becomes "change TTBR0 (table + ASID), done."

Builds on Phase 6b (per-process address spaces) and Phase 9 (COW).

## How the hardware ties ASID to translation

Three pieces have to agree:

1. **The current ASID lives in `TTBR0_EL1[63:48]`** (the high 16 bits of the same
   register that holds the page-table base). So loading TTBR0 on a switch sets
   *both* the tables and the ASID atomically.
2. **`TCR_EL1.AS` (bit 36) selects the ASID width:** 0 = 8-bit, 1 = 16-bit. We
   use **16-bit** (65 535 usable IDs — 8-bit's 255 is uncomfortably few).
   `TCR_EL1.A1` (bit 22) stays 0, meaning `TTBR0`'s ASID field is the one in
   force (we only use the low/TTBR0 half).
3. **Only *non-global* leaf entries are tagged.** Each page/block descriptor has
   an **nG bit (bit 11)**: nG=0 → *global* (matches under any ASID), nG=1 →
   *non-global* (tagged with the current ASID). Today nothing sets nG, so every
   entry is global — which is exactly wrong for isolation once we stop flushing.
   We must mark **user pages non-global** while **kernel mappings stay global**.

### Why kernel stays global, user goes non-global

The kernel is identity-mapped in the low half (`l2_high`, EL1-only) and shared
into every address space via `l0[0]`. Those mappings are identical no matter
which process runs, so leaving them **global (nG=0)** means they're never
flushed and never duplicated per-ASID — a real win, since the kernel is touched
on every trap. User code/stack/data pages differ per process, so they're
**non-global (nG=1)**, tagged by ASID, and isolated automatically.

## ASID allocation (`vm.c`)

A monotonic counter, ID 0 reserved (never handed out — it's the "unused / boot
TTBR0" value):

```c
#define ASID_MAX 0xFFFF
static uint32_t next_asid = 1;        // reset to 1 by vm_init()

uint16_t asid_alloc(void) {
    if (next_asid > ASID_MAX) {       // rollover: we've handed out every ID
        next_asid = 1;
        flush_all_tlb();              // tlbi vmalle1 — drop every stale tagged entry
    }
    return (uint16_t)next_asid++;
}
```

**Why rollover needs a full flush:** once we wrap and start *reusing* an ID, the
TLB may still hold a long-dead address space's entries under that same ID. One
`tlbi vmalle1` at wrap time clears them, so a recycled ID is always clean. This
is the classic simple ASID scheme; generation/rolling-window allocators (Linux)
are an optimization we don't need at this scale.

`struct addrspace` gains a field:
```c
struct addrspace { uint64_t *l0; uint16_t asid; };
```
`as_create_image` and `as_clone` each set `as->asid = asid_alloc()`. A clone is a
**distinct** address space (its COW pages must not alias the parent's in the
TLB), so it gets its **own** ASID — not the parent's.

## Switching without a flush (`as_switch`)

```c
void as_switch(struct addrspace *as) {
    uint64_t ttbr = ((uint64_t)as->asid << 48) | (uint64_t)(uintptr_t)as->l0;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr));
    __asm__ volatile("isb");
    // NO tlbi here — the ASID keeps the old process's entries from matching.
}
```

## Targeted invalidation (`as_clone`, `cow_fault`)

When we *change* a live address space's mappings we must drop just **that ASID's**
stale entries, not the whole TLB. `tlbi aside1, Xt` invalidates entries whose tag
equals `Xt[63:48]`:

```c
static void flush_tlb_asid(uint16_t asid) {
    uint64_t arg = (uint64_t)asid << 48;
    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi aside1, %0" :: "r"(arg));
    __asm__ volatile("dsb ish; isb");
}
```

- `cow_fault(as, va)` remaps one page in `as` → `flush_tlb_asid(as->asid)`.
- `as_clone(parent)` demotes the **parent's** writable pages to read-only+COW →
  `flush_tlb_asid(parent->asid)` (the child isn't live yet, so it has no TLB
  entries to flush).

`flush_all_tlb()` (the old `tlbi vmalle1`) is kept only for ASID rollover.

## TCR_EL1 change (`mmu.c`)

Add `TCR_EL1.AS` (bit 36 = 1) for 16-bit ASIDs to the `tcr` value in
`enable_mmu()`. Kernel block descriptors in `mmu.c` already leave nG=0 (global) —
no change needed there.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/vm.h`/`vm.c` | `asid` field; `asid_alloc`; nG on user pages; `as_switch` carries ASID; `flush_tlb_asid`/`flush_all_tlb`; rollover |
| `src/mmu.c` | `TCR_EL1.AS = 1` (16-bit ASIDs) |
| `src/tests.c` | ASID tests (test-first) |
| `docs/notes/phase-11.md` | notes |

## Testing (test-first, deterministic)

Directly observing TLB hits/misses needs performance counters we don't have, so
we test the **machinery's observable invariants** — the things that, if wrong,
break isolation once flushing is removed:

1. `test_asid_assigned_nonzero` — `as_create()` yields `as->asid != 0`.
2. `test_asid_unique` — two address spaces get **different** ASIDs.
3. `test_asid_clone_distinct` — `as_clone(parent)->asid != parent->asid`.
4. `test_asid_user_page_nonglobal` — a user page PTE has nG (bit 11) **set**
   (so it's ASID-tagged). Checked via a small `as_pte(as, va)` accessor.
5. `test_asid_rollover_recycles` — after `vm_init()` then `ASID_MAX` allocations,
   the next `asid_alloc()` wraps back to `1` (and never returns `0`).

(The flush-free switch itself is exercised by the existing scheduler/COW tests
and the live shell, which keep working with isolation intact.)

## Success criteria

- 5 ASID tests pass (test-first); all prior tests stay green; the shell still
  boots and runs (isolation holds with no per-switch flush); gate intact.

## Out of scope

ASID lifetime/recycling on process *exit* (we never reap processes yet);
generation-based rolling-window allocation; per-CPU ASID pools (single core);
TTBR1 / split kernel-user translation (kernel stays in TTBR0, global).
