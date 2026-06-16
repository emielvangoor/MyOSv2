# ext2 as Root Filesystem — Design

**Date:** 2026-06-16
**Status:** Approved (pending spec review)
**Goal:** Make the MyOSv2 root filesystem `/` a real, persistent ext2 image, so on-device edits (`/hello.c`, files created with `C-x C-f`, edited Lisp) survive reboots — the Linux model, where the build *installs* the system onto a disk and the kernel mounts it.

---

## Problem

Today the root `/` is **ramfs** (in-RAM), rebuilt every boot:

- `kmain.c` calls `vfs_mount_root(ramfs_type())` then `initrd_unpack()`, which writes files compiled into the kernel (`user_blob.o` = ELF binaries, `lisp_blob.o` = `.l` source, plus `/hello.c`) into the empty ramfs.
- Only `/disk` is persistent ext2 (`vfs_mount_at("/disk", ext2_mount())`).

So editing `/hello.c` (or any non-`/disk` file) changes only the RAM copy. On reboot ramfs starts empty and `initrd_unpack()` re-creates the original from the embedded `hello_c[]` string — the edit is lost. Files written under `/disk/...` already persist; everything else is ephemeral.

`C-x C-f` create-on-save already works (verified by `findfile_check.py` and `frameedit_check.py`): visiting a missing path gives an empty buffer, and `save-buffer` → `creat` (`openat` with `O_CREAT`) creates it. The missing piece is **not** create-on-save — it is a persistent root.

## Solution: the Linux model

The disk image **is** the installed root filesystem. The build "installs" all system files onto the ext2 image; the kernel mounts it as `/`; from then on it just persists. There is **no** kernel-side unpacking, **no** overwrite-on-boot, **no** ramfs/initrd hybrid. Recompiling the OS and regenerating the image is the equivalent of reflashing — the running machine never touches what is on disk except when the user saves.

### Persistence semantics (the behavior we are buying)

- Edit `/hello.c`, save, reboot → the edited version is still there.
- `C-x C-f notes.txt` (any path) + `C-x C-s` → created on disk, survives reboot.
- Regenerating `disk.img` (a `make` that rebuilds it, or `make clean`) is a deliberate "reflash" that resets system files. Nothing on the running machine resets them.

---

## Component 1 — Build: "install onto the disk"

`build/rootfs/` becomes the **complete root tree**, and `mke2fs -t ext2 -d build/rootfs` bakes it into `disk.img`. Today `rootfs` holds only `init.l`, the `test/` fixtures, and the musl sysroot. We add everything the initrd used to unpack:

| Destination on disk | Source | Notes |
|---|---|---|
| `rootfs/bin/init`, `rootfs/bin/lisp` | `build/user/lm.elf` | same binary at both names (as initrd did) |
| `rootfs/bin/<prog>` | `build/user/<prog>.elf` | for every `PROGS`, `MUSL_PROGS`, `PREBUILT_PROGS` (incl. **busybox**, **tcc**) |
| `rootfs/lib/<f>.l` | `user/lisp/<f>.l` | for every `LISP_FILES` (`bootstrap system modes fr-* frame`) |
| `rootfs/lib/mycrt.o` | `build/user/mycrt.elf` | crt the on-device tcc links against |
| `rootfs/hello.c`, `rootfs/hellobare.c` | the seed sources (moved out of `initrd.c`) | seed files; persist once edited |
| `rootfs/usr/include`, `rootfs/usr/lib` | musl sysroot | **moves from `/disk/usr` to `/usr`** (already staged, just relocates) |
| `rootfs/init.l`, `rootfs/test/*` | as today | unchanged |

`PROGS` = `sh true false hello mtest shmtest wc loop catch ping dnsq http httpd polldemo lm evtest gfxtest surftest fptest teapot`; `MUSL_PROGS` = `mhello mmalloc mfork mfile`; `PREBUILT_PROGS` = `busybox tcc`.

### Kernel stops embedding the binaries

Because the binaries and Lisp now live on disk, the kernel no longer embeds or unpacks them:

- Remove `$(BUILD)/user_blob.o` and `$(BUILD)/lisp_blob.o` from the kernel `OBJ` list (and the `user_blob.c` / `lisp_blob.c` xxd recipes).
- Remove **`src/initrd.c`** and its call site (the seed sources `hello.c`/`hellobare.c` move into the disk-image recipe as plain files).
- The `.elf` build rules and `libtcc1.a` rule stay (the disk image now depends on them instead of the blob steps depending on them).

Net effect: a smaller kernel, a single source of truth for the userland, and the standard Unix layout (`/bin`, `/lib`, `/usr`).

## Component 2 — Boot: "mount root, exec init"

In `kmain.c`, the filesystem section changes to:

1. `virtio_blk_init()` — **moved up**, before mounting root (the root now lives on the block device).
2. `vfs_mount_root(ext2_type())` — mount ext2 as `/`. (Add a `fs_type` wrapper around the existing `ext2_mount()` so it matches `vfs_mount_root`'s `struct fs_type *` interface, mirroring `ramfs_type()`.)
3. If the mount fails (blank/corrupt/non-ext2 disk → `ext2_mount()` returns NULL), `panic("VFS: unable to mount root fs")` — the Linux behavior, rather than limping on a NULL root.
4. Remove `initrd_unpack()` and the separate `vfs_mount_at("/disk", ...)` mount.

Every QEMU invocation (`run`, `test`, `debug`, and `tools/lm_harness.py`) already attaches a virtio-blk disk, so a disk is always present — no diskless fallback is needed in practice; the panic covers the genuinely-broken-disk case.

`proc_spawn("/bin/init", 2)` (already after the mount) loads `/bin/init` from the ext2 root unchanged.

## Component 3 — Path updates (the `/disk` → `/` move)

- **`user/lisp/system.l`**, the `cc` link line: `/disk/usr/include` → `/usr/include`, `/disk/usr/lib/...` → `/usr/lib/...`. `cc-bare` already uses `/lib/mycrt.o` (unchanged).
- **Boot-counter demo** in `kmain.c`: `/disk/boots` → `/boots`. It is now just an ordinary persistent file; kept as a one-line "we are persistent" proof.
- `init.l` stays at `/init.l`; `run-bg "lisp" "-frame"` unchanged.

## Component 4 — ramfs

`src/ramfs.c` / `ramfs.h` stay in the tree but are no longer mounted. (A future `/tmp` could mount ramfs — explicitly **out of scope** here.) Leaving the code in place keeps the diff focused on the root-mount change and preserves the option.

---

## Testing

**TDD throughout** (`make test` gates commits via the pre-commit hook). The suite must stay green and the new behavior must be covered.

### KTESTs (`src/tests.c`, in-kernel via crafted trapframes)

- **Risk:** any KTEST that reads an old initrd-only path (`/hello.txt`, `/motd`) will now find nothing, because those came from `initrd_unpack`. Resolution: stage `hello.txt` and `motd` into `build/rootfs` so they exist on the disk image, **or** update the test to use a path that exists. Audit `src/tests.c` for hardcoded initrd paths before cutover.
- Existing ext2 read/write KTESTs (raw sectors at 100000+, file create/write/grow/truncate/unlink) are unaffected — they target the same driver.

### New integration test — persistence across reboot (the headline behavior)

`tools/persist_check.py` (new): boot under the harness (which copies `build/disk.img` to a private scratch image), then:

1. In a REPL window, write a sentinel to a root-path file:
   `(let ((fd (creat "/persist-test.txt"))) (fd-write fd "PERSISTED-OK") (close fd))`.
2. Cleanly stop QEMU, **reusing the same scratch image** (do not re-copy from `build/disk.img`).
3. Boot again against that same scratch image.
4. `(cat "/persist-test.txt")` → assert the serial shows `PERSISTED-OK`.

This requires a small harness addition: a way to run a second QEMU against the **same** scratch disk (the current harness copies a pristine image each run; the test needs persistence across two boots of one image). Add an opt-in flag/parameter to `lm_harness.Qemu` to skip the re-copy.

### Existing integration checks

`findfile_check.py`, `frameedit_check.py`, `printf_check.py`, etc. boot from a copy of `disk.img` and inherit the new layout for free. `frametcc_check.py` / `printf_check.py` exercise `cc`, which now uses `/usr/...` — they validate the path move end-to-end. Host `e2fsck -fn` on the post-write scratch image stays part of the verification.

---

## Out of scope

- Mounting ramfs (or any fs) at `/tmp`.
- Multiple disks / a separate persistent `/disk` in addition to root.
- A first-boot "installer" that formats a blank disk on the device (the host `mke2fs` build step is the installer).
- Any change to the ext2 on-disk driver itself (read/write already complete).
