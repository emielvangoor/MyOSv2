# Phase 14 notes — ELF64 loader + coreutils

## What changed

The kernel loads real **ELF64** executables, and several programs now live under
`/bin`. The shell's fork→exec→wait path runs them and shows their exit status:

```
$ hello
Hello from /bin/hello
[exit 0]
$ true
[exit 0]
$ false
[exit 1]
$ nope
exec: not found
[exit 127]
```

The shell itself is now loaded via the ELF path.

## The ELF loader

An ELF executable's header points at a **program-header table**; each `PT_LOAD`
entry says "map these bytes at this virtual address." `elf_load` validates the
magic / 64-bit class / AArch64 machine, then for each `PT_LOAD` calls
`as_map_segment(vaddr, src, filesz, memsz, writable, exec)`.

`as_map_segment` maps the page-aligned range covering `[vaddr, vaddr+memsz)`:
each page is freshly allocated and **zeroed**, then the overlapping slice of
`[src, src+filesz)` is copied in — so the `.bss` tail (the `memsz - filesz`
beyond the file bytes) is zero for free. Permissions come from the segment flags:
executable → RO + EL0-exec; writable → RW + no-exec.

`as_create_elf` ties it together: a fresh top-level table sharing the kernel map
(`as_alloc_l0`), the ELF segments, plus the 16-page user stack below
`USER_STACK_TOP`. It returns the entry point (`e_entry`), which
`thread_create_image` and `proc_exec` use as the EL0 start address (instead of
the old fixed `USER_CODE_VA`).

The linker script page-aligns `.data` (`. = ALIGN(4096)`) so writable data lands
in its own LOAD segment and can be mapped RW without making code writable. Our
current programs have no writable globals, so they ship as a single R+E segment;
the loader handles one or many.

## The multi-program build

`PROGS := sh true false hello`. Each is linked as its own ELF from
`crt0 + ulib + <prog>.c`, turned into a C byte array with `xxd -i` (giving
`<prog>_elf` / `<prog>_elf_len`), and concatenated into `user_blob.c`. `initrd.c`
unpacks them to `/bin/init`, `/bin/sh`, `/bin/true`, `/bin/false`, `/bin/hello`.

`umain` now returns an `int` that `crt0` passes to `SYS_EXIT` — so a program's
return value *is* its exit status (`true`→0, `false`→1), which the shell prints.

## Testing

4 loader tests, test-first: `as_map_segment` zeroes the `.bss` tail; `elf_load`
rejects non-ELF bytes; a hand-built minimal ELF loads with the right entry and
segment bytes; `as_create_elf` of the embedded shell maps both a code page and a
stack page. Real programs executing is verified live in the shell.

## Limits / next

No `argv`/`envp` yet — so arg-taking utilities (`echo args`, `cat <file>`, `ls
<dir>`) await a small follow-on that writes an argv block onto the new program's
stack and passes `argc`/`argv` in `x0`/`x1`. No dynamic linking, `PT_INTERP`, or
relocations (programs are linked at their final vaddr). The flat
`as_create_image`/`as_create` remain as test-only helpers.
