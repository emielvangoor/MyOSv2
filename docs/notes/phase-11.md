# Phase 11 notes — ASIDs (tagged TLB entries)

## The problem

Every context switch used to run `tlbi vmalle1` in `as_switch` — it threw away
**all** cached translations, kernel and every process alike. So right after each
switch the CPU had to re-walk the page tables for every access until the TLB
refilled. With ~hundreds of switches a second, that's a lot of wasted walks,
including for the kernel mappings that never change.

## How ASIDs fix it

An **ASID** (Address-Space IDentifier) is a small integer naming one address
space. The CPU **tags** each TLB entry it caches with the ASID that was active,
and only *uses* an entry whose tag matches the **current** ASID. So process A's
entries simply lie dormant while process B runs — no flush needed to keep them
apart. A switch becomes "load the new ASID + table base," nothing more.

Three pieces have to agree:

1. **The current ASID lives in `TTBR0_EL1[63:48]`** — the top 16 bits of the same
   register holding the table base. So `as_switch` writes
   `(asid << 48) | l0_phys` and sets both at once, then `isb`. **No `tlbi`.**
2. **`TCR_EL1.AS` (bit 36) = 1** selects **16-bit** ASIDs (65 535 IDs; 8-bit's
   255 is too few). `TCR_EL1.A1` stays 0 → `TTBR0`'s ASID is the one in force.
3. **The nG bit (bit 11) in a leaf descriptor** decides tagging: nG=0 → *global*
   (matches under any ASID), nG=1 → *non-global* (tagged by the current ASID).

## Global kernel, non-global user

- **Kernel mappings stay global (nG=0).** The kernel is identity-mapped EL1-only
  and shared into every address space via `l0[0]`; it's identical no matter who
  runs, so global means it's never flushed and never duplicated per-ASID.
- **User pages are non-global (nG=1).** Code/stack/data differ per process, so
  they're tagged by ASID and isolated automatically. `ATTR_NG` is now OR'd into
  every user attribute set (`as_create_image`, `as_clone`, `cow_fault`).

This split is *why* dropping the per-switch flush is safe: distinct ASIDs + nG
user entries mean B can never match A's translations, while the global kernel
entries survive the switch untouched.

## Allocator + rollover

```c
#define ASID_MAX 0xFFFF
static uint32_t next_asid = 1;     // 0 reserved (boot/unused TTBR0 value)
uint16_t asid_alloc(void) {
    if (next_asid > ASID_MAX) { next_asid = 1; flush_all_tlb(); }  // recycle
    return (uint16_t)(next_asid++);
}
```

A monotonic counter; `vm_init()` resets it to 1. ID 0 is never handed out.
`as_create_image` and `as_clone` each take one — a clone is its own address space
(its COW pages must not alias the parent's in the TLB), so it gets its **own**
ASID, not the parent's. On rollover we wrap to 1 and do **one** full
`tlbi vmalle1`: reusing an ID is only safe once any dead space's stale entries
under that ID are gone. (Real kernels use generation/rolling-window allocators;
overkill at this scale.)

## Targeted invalidation

When we change a *live* address space's mappings we drop just **that ASID's**
entries, not the whole TLB, via `tlbi aside1, Xt` (Xt[63:48] = asid):

- `cow_fault(as, …)` remaps one page → `flush_tlb_asid(as->asid)`.
- `as_clone(parent)` demotes the parent's writable pages to read-only+COW →
  `flush_tlb_asid(parent->asid)` (the child has no live entries yet).

`flush_all_tlb()` (the old `vmalle1`) now survives only for rollover.

## Testing

5 tests pin the machinery's observable invariants (TLB hit/miss counters aren't
available to test directly): ASID assigned non-zero; two spaces get distinct
IDs; a clone gets its own ID; a user PTE has nG set; and the allocator wraps back
to 1 after `ASID_MAX` allocations. The flush-free switch itself is exercised by
the existing scheduler/COW tests and by booting the live shell — isolation holds
with no per-switch flush.

## Limits

No ASID recycling on process *exit* (we don't reap processes yet), so a workload
that spawns >65 535 processes triggers a rollover flush; no generation allocator;
single-core (no per-CPU ASID pools); kernel stays in TTBR0 (no TTBR1 split).
