# MyOSv2 — Phase 14 Design (ELF64 loader + coreutils)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Load real **ELF64** executables — parse the program headers, map each `PT_LOAD`
segment at its own virtual address with correct permissions (RX code, RW data),
zero-fill `.bss` — and ship several separately-built programs under `/bin`. The
shell's Phase-13 fork→exec→wait path then runs them for real, showing their exit
status.

Builds on the process lifecycle (13) and address spaces (6b).

## Why ELF (vs the flat blob)

Today the single user program is a flat binary mapped wholesale at `USER_CODE_VA`
read-only — writable globals would fault, and there's no real `.bss`. ELF gives
each segment its own vaddr and permissions, so programs can have writable data,
and multiple distinct programs can be loaded properly. It's also the universal
format, so this is a reusable capability.

## ELF loader (`elf.c`)

ELF64 little-endian, machine `EM_AARCH64` (0xB7). We read two structures
(packed): the header `Elf64_Ehdr` (entry `e_entry`, program-header table
`e_phoff`/`e_phnum`/`e_phentsize`) and each `Elf64_Phdr` (`p_type`, `p_flags`,
`p_offset`, `p_vaddr`, `p_filesz`, `p_memsz`).

```c
int elf_load(struct addrspace *as, const void *img, uint64_t len, uint64_t *entry);
```
- Validate magic (`\x7fELF`), 64-bit class, AArch64 machine. Bad → -1.
- `*entry = e_entry`.
- For each `PT_LOAD` (type 1): `as_map_segment(as, p_vaddr, img + p_offset,
  p_filesz, p_memsz, writable = p_flags & PF_W, exec = p_flags & PF_X)`.
- Return 0.

## Segment mapping (`vm.c`)

```c
void as_map_segment(struct addrspace *as, uint64_t vaddr,
                    const void *src, uint64_t filesz, uint64_t memsz,
                    int writable, int exec);
```
Maps the page-aligned range covering `[vaddr, vaddr+memsz)`. Each page is freshly
allocated and **zeroed**, then the overlapping slice of `[src, src+filesz)` is
copied in (so the `.bss` tail beyond `filesz` stays zero). Attributes:
- exec (code): `AF | SH | NORMAL | AP_RO_ALL | NG` (EL0-executable, read-only).
- writable (data): `AF | SH | NORMAL | AP_RW_ALL | UXN | PXN | NG`.

Segments are **page-aligned by the linker** (`. = ALIGN(4096)` before `.data`),
so no page is shared between segments with conflicting permissions.

```c
struct addrspace *as_create_elf(const void *img, uint64_t len, uint64_t *entry);
```
Builds a fresh AS (kernel L0[0] + ASID), `elf_load`s the segments, adds the
16-page user stack below `USER_STACK_TOP`, and returns it (entry via `*entry`).
The flat `as_create_image`/`as_create` stay as **test helpers** for the existing
vm/cow/asid tests.

## Wiring program loading to ELF

- `thread_create_image(img, len, prio)` → `as_create_elf` and start at the ELF
  entry (instead of the fixed `USER_CODE_VA`).
- `proc_exec(tf, path)` → `as_create_elf`; `tf->elr = entry`.

## Exit status from `main` (`crt0.S`, `ulib`)

`umain` returns an `int` used as the exit status:
```c
int umain(void);          // was void umain(void)
```
`crt0.S`: `bl umain` then `mov x8,#SYS_EXIT; svc #0` with `x0` = umain's return
value. So `true` returns 0, `false` returns 1, and the shell prints the status.

## Build system (`Makefile`) + initrd

Build several programs, each as a **separate ELF** (no objcopy), from
`crt0.S + ulib.c + <prog>.c`:
```
PROGS := sh true false hello
```
Each `build/user/<p>.elf` is turned into a C byte array with `xxd -i` (names
`<p>_elf` / `<p>_elf_len`), concatenated into `user_blob.c`. `initrd.c` registers
them under `/bin`: `/bin/init` and `/bin/sh` = `sh_elf`; `/bin/true`,
`/bin/false`, `/bin/hello`. `vm.c`'s flat test loader switches its extern from
`init_bin` to `sh_elf` (content-agnostic — those tests only check page mappings).

## Coreutils (`user/`)

- `true.c`  — `int umain(void){ return 0; }`
- `false.c` — `int umain(void){ return 1; }`
- `hello.c` — prints "Hello from /bin/hello\n", returns 0.
- `sh.c` — `umain` returns int (it loops, so never actually returns).

The shell's `run_external` (Phase 13) already execs `/bin/<cmd>` and waits, so
typing `true`, `false`, or `hello` now runs the real ELF program and prints its
status.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/elf.c`/`.h` | ELF64 parse + `elf_load` |
| `src/vm.c`/`vm.h` | `as_map_segment`, `as_create_elf` |
| `src/sched.c` | `thread_create_image` uses ELF entry |
| `src/proc.c` | `proc_exec` uses ELF entry |
| `user/crt0.S`, `user/ulib.h` | `int umain` → exit status |
| `user/true.c`/`false.c`/`hello.c` | new coreutils |
| `user/user.ld` | page-align `.data` for a separate RW segment |
| `Makefile` | build N programs, embed each |
| `src/initrd.c` | register `/bin/*` programs |
| `src/tests.c` | ELF loader tests (test-first) |
| `docs/notes/phase-14.md` | notes |

## Testing (test-first, deterministic)

The loader is unit-tested by constructing a minimal ELF image in memory; the
live run confirms real programs execute:

1. `test_elf_rejects_bad_magic` — `elf_load` on non-ELF bytes returns -1.
2. `test_elf_entry_and_segment` — a hand-built ELF (one `PT_LOAD` at a known
   vaddr with a few bytes) loads: `*entry` matches `e_entry`, and
   `as_translate(as, vaddr)` resolves to a page whose bytes equal the segment
   data.
3. `test_as_map_segment_bss_zeroed` — `as_map_segment` with `filesz < memsz`
   copies the file part and leaves the `.bss` tail (beyond `filesz`) zero.
4. `test_as_create_elf_has_stack` — `as_create_elf` of the embedded `sh_elf`
   maps a private stack page just below `USER_STACK_TOP`.

## Success criteria

- 4 loader tests pass (test-first); all prior tests stay green; gate holds.
- Live: the shell runs `true`/`false`/`hello` as real ELF programs and prints
  their exit status; `/bin` lists them; the shell itself loads via the ELF path.

## Out of scope

`argv`/`envp` (so arg-taking utils like `echo`/`cat <file>` wait for a small
follow-on that writes argv onto the new stack); dynamic linking / shared
libraries; `PT_INTERP`; relocations (programs are linked at their final vaddr).
