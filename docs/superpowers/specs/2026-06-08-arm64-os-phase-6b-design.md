# MyOSv2 — Phase 6b Design (Per-Process Address Spaces)

**Date:** 2026-06-08
**Status:** Approved (autonomous build authorized)

## Goal

Give each user process its **own page tables** (its own `TTBR0`), so different
processes have **different physical memory at the same user virtual address** —
true isolation. Completes the user-mode work begun in 6a (which shared one
address space). The kernel mapping stays shared (so traps/syscalls work); user
**data/stack** become private; user **code** is shared read-only (like a real
text segment).

Builds on 6a (EL0, syscalls, SP_EL0 banking) and Phase 3 (page tables).

## Address space layout

Each process has an `addrspace` with its own top-level L0 table:

```
TTBR0 -> as->l0 +- l0[0] -> SHARED kernel L1  (identity 0-2GB, EL1-only)  [from mmu.c]
                +- l0[1] -> PRIVATE user L1 -> user L2 -> user L3 (4 KiB pages):
                              USER_CODE_VA   -> shared code pages (RO, EL0-exec)
                              USER_STACK ...  -> private stack pages (EL0 RW)
                              USER_DATA_VA   -> private data page  (EL0 RW)
```

- `l0[0]` is the **same** kernel L1 in every process (shared identity mapping,
  EL1-only) — the kernel works no matter which process is active. Obtained from
  `mmu_kernel_l0_entry()` (exposes `l0_table[0]` from `mmu.c`).
- `l0[1]` is **private** per process. User VAs live at ≥512 GiB
  (`0x80_0000_0000`), a separate L0 entry from the kernel's, mapped with **4 KiB
  pages** (L3) since page-table pages come from the PMM (4 KiB-aligned).
- **Code is shared**, **stack/data are private**. Code pages hold a copy of the
  user program; stack/data pages are freshly PMM-allocated per process.

Fixed user VAs:
- `USER_CODE_VA  = 0x80_0000_0000`
- `USER_STACK_TOP= 0x80_0040_0000` (a few private pages below it)
- `USER_DATA_VA  = 0x80_0080_0000` (one private page)

## The user program is relocated (a tiny loader)

User code must run at `USER_CODE_VA`, not its link-time kernel address. To make
that safe:
- All user code goes in a dedicated **contiguous `.user` linker section** with
  `__user_start`/`__user_end` symbols (functions tagged
  `__attribute__((section(".user")))`).
- The user code is **string-literal free** (no `.rodata` dependence), so it only
  uses PC-relative branches within `.user` — copying the contiguous blob to
  `USER_CODE_VA` and running there is correct.
- `as_create()` copies `[__user_start, __user_end)` into the shared code pages
  and maps them at `USER_CODE_VA`. The user entry is
  `USER_CODE_VA + (user_main - __user_start)`.

## Components

### `addrspace` (`vm.h` / `vm.c`)

```c
struct addrspace { uint64_t *l0; };   // l0 is a PMM page

struct addrspace *as_create(void);            // build kernel-shared + private user maps
uint64_t          as_translate(struct addrspace *as, uint64_t va); // walk -> PA (0 if unmapped)
void              as_switch(struct addrspace *as);                 // TTBR0 = as->l0; flush TLB
```

- `as_create`: alloc L0 (PMM); `l0[0] = mmu_kernel_l0_entry()`; build private
  user L1/L2/L3 (PMM pages); map shared code pages (copied blob, AP RO EL0-exec),
  private stack pages and a private data page (AP=01 EL0 RW).
- `as_translate`: walk L0→L1→L2→L3 (or block) and return the physical address a VA
  maps to (or 0). Used by tests to prove isolation at the table level.
- `as_switch`: `msr ttbr0_el1, as->l0`; `tlbi vmalle1`; `dsb ish`; `isb`.

### Scheduler integration (`sched.c`)

`struct thread` gains `struct addrspace *as` (NULL for kernel threads).
`schedule()`, when it switches to a thread, calls `as_switch(next->as)` if
`next->as` is non-NULL (and tracks the current AS to avoid redundant switches /
restore the kernel tables for kernel threads). `thread_create_user` becomes:
allocate an `addrspace`, enter the user thread at `USER_CODE_VA + entry_off` with
`SP_EL0 = USER_STACK_TOP`.

### A reporting syscall (`syscall.c`)

`SYS_REPORT (5)`: `x0 = pid`, `x1 = value` → the kernel prints
`process <pid> read <value>` and returns 0. Lets the (string-free) user program
make the isolation result visible.

### MMU change (`mmu.c`)

Remove the global 6a EL0 alias (`l2_user` / `l1_table[2]`); user memory is now
per-process. Expose `uint64_t mmu_kernel_l0_entry(void)` returning `l0_table[0]`.

## The user program (`user.c`, in `.user`, string-free)

```
user_main():
  pid   = SYS_GETPID
  *(USER_DATA_VA) = pid          // write our pid to our PRIVATE data page
  SYS_SLEEP(150)                  // ...the other process runs and writes ITS pid
  seen  = *(USER_DATA_VA)         // read back
  SYS_REPORT(pid, seen)          // kernel prints; isolated => seen == pid
  SYS_EXIT
```

## Demo (`kmain`)

Launch **two** user processes (same code, separate address spaces) plus a kernel
thread. Each writes its pid to `USER_DATA_VA`, sleeps, reads back, and reports.
Isolated → each reports `read == pid` (process 1 reads 1, process 2 reads 2). If
they shared memory, the later writer would clobber the earlier and a process
would read the other's pid.

## Testing (test-first, deterministic — at the page-table level)

EL0 isolation can't be unit-tested directly, but the **mapping** can:

1. `test_as_data_is_private` — two `as_create()`s map `USER_DATA_VA` to
   **different** physical pages.
2. `test_as_code_is_shared` — both map `USER_CODE_VA` to the **same** physical
   page.
3. `test_as_kernel_shared` — both translate a kernel VA (e.g. `0x40080000`) to
   itself (identity; shared kernel mapping present).
4. `test_as_unmapped_returns_zero` — an unmapped VA translates to 0.
5. `test_as_stack_is_private` — two ASs map a `USER_STACK` VA to different pages.

The actual EL0 two-process isolation is observed in the demo. All prior tests
stay green.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/vm.h` / `vm.c` | `addrspace`, `as_create`/`as_translate`/`as_switch` |
| `src/mmu.h` / `mmu.c` | drop global alias; expose `mmu_kernel_l0_entry()` |
| `src/sched.h` / `sched.c` | thread `as`; `as_switch` on context switch; user-thread creation |
| `src/syscall.h` / `syscall.c` | `SYS_REPORT` |
| `src/user.c` | string-free user program in `.user`; pid private-page write/read |
| `linker.ld` | `.user` section + `__user_start`/`__user_end` |
| `src/tests.c` | 5 address-space mapping tests (test-first) |
| `src/kmain.c` | two isolated user processes + kernel thread |
| `docs/notes/phase-6b.md` | document per-process address spaces |

All new code heavily commented.

## Success criteria

- 5 new `as_*` mapping tests pass (test-first); all prior tests green; `make test`
  exit 0; gate intact.
- Demo: two user processes each report `read == own pid` (isolation holds); they
  run concurrently with a kernel thread under preemption.

## Out of scope

Demand paging, `fork`/`exec`, copy-on-write, ASIDs (we flush the whole TLB on
switch), shared memory between processes, swapping, ELF parsing (we copy a flat
`.user` blob).
