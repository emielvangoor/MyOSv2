# ext2 filesystem + musl sysroot + on-device printf — design

**Goal:** replace MyOSv2's toy persistent filesystem (SFS, 6 KB max file) with a
real, POSIX, on-disk **ext2** read+write driver, then use it to host a **musl
libc sysroot** on `/disk` so the on-device compiler (`tcc`) can build programs
that `#include <stdio.h>` and call `printf` — verified end to end.

North star: `(cc "/hello.c" "/hello")` where `hello.c` is `#include <stdio.h>` /
`printf("x=%d\n", 42)` produces a runnable binary that prints `x=42`.

## Why ext2

ext2 is the canonical real Unix filesystem: inodes with **indirect blocks**
(multi-MB files — `libc.a` now, `gcc` later), Unix permissions/timestamps, real
directories, hard links. It is exhaustively documented, has no journaling/extents
to implement (that's ext3/4), and — decisively — the **host can build and
pre-populate images**: `e2fsprogs` is installed (`/opt/homebrew/opt/e2fsprogs`),
and `mke2fs -d <dir>` creates an image already containing a directory tree, so
the musl sysroot is baked into `/disk` at build time. SFS can't hold anything
over 6 KB (12 direct blocks, no indirect), which is the root limitation this
replaces.

## Current state (what this builds on / replaces)

- **Block layer:** `block_read(blk, buf)` / `block_write(blk, buf)`, 512-byte
  sectors (virtio-blk). SFS and the new driver sit on top.
- **VFS:** filesystems implement `struct vnode_ops` (read/write/lookup/create/
  readdir/truncate) and register a `struct fs_type { name; mount; }`. `ramfs` is
  the in-RAM root (initrd unpacked into it); `vfs_mount_at("/disk", X)` mounts a
  second FS. `vfs_lookup` walks path components, calling `ops->lookup` per
  component, and matches mount prefixes — so nested dirs and `/disk/...` paths
  work for any `fs_type`.
- **`/disk` today:** `vfs_mount_at("/disk", sfs_mount())` in `kmain.c`; SFS
  auto-`mkfs`'s a zeroed disk. `make test` and `tools/lm_harness.py` rely on a
  zeroed 4 MB image being a valid (blank) SFS.
- **On-device compile:** `/bin/tcc` (static-musl TinyCC) + `mycrt.S` (freestanding
  `_start`/`puts`/`print`). `cc`/`run-file` Lisp helpers. Programs are static,
  `-no-pie`, linked at `0x8000000000`. The kernel already runs static-musl
  binaries (busybox, mhello) — `crt1.o` + `__libc_start_main` + `printf` work.

## Architecture & components

- **`src/ext2.h` (new)** — on-disk structures (superblock, group descriptor,
  inode, directory entry) and `struct fs_type *ext2_type(void)` / `struct vnode
  *ext2_mount(void)`.
- **`src/ext2.c` (new)** — the read+write driver, behind `vnode_ops`. Sole
  consumer of `block_read`/`block_write` for `/disk`.
- **`src/kmain.c`** — mount ext2 at `/disk` instead of SFS.
- **`src/sfs.c`, `tools/mkdisk.py`** — **removed**. `sfs:` KTESTs become
  `ext2:` KTESTs.
- **`Makefile`** — `disk.img` is built by `mke2fs -F -b 1024 -d build/rootfs`;
  `build/rootfs/` staging holds `init.l` + the musl sysroot.
- **`tools/lm_harness.py`** — per-run scratch disk is a **copy of the built
  ext2 `build/disk.img`** (not a zeroed truncate), so the frame/serial checks
  mount a valid ext2 filesystem.
- **`user/lisp/system.l`** — `cc` becomes the musl link line (§ tcc).
- **`src/initrd.c`** — default `/hello.c` becomes `#include <stdio.h>` + printf.

## ext2 on-disk format (the implemented subset)

**Block size 1024** (mke2fs default at ≤512 MB; forced with `-b 1024`). One ext2
block = 2 sectors; an `eb_read(block, buf)`/`eb_write(block, buf)` helper does the
2-sector transfer. `first_data_block = 1` for 1024-byte blocks (the superblock
lives in block 1, at byte offset 1024).

Structures (little-endian, packed):
- **Superblock** (at byte 1024): `s_inodes_count`, `s_blocks_count`,
  `s_first_data_block`, `s_log_block_size` (0 ⇒ 1024), `s_blocks_per_group`,
  `s_inodes_per_group`, `s_magic` (`0xEF53`), `s_inode_size` (128 or 256;
  `s_inode_size` lives in the ext2-rev-1 area, default 128 for old, 256 common).
- **Block group descriptor** (table starts at the block after the superblock):
  per group `bg_block_bitmap`, `bg_inode_bitmap`, `bg_inode_table`,
  `bg_free_blocks_count`, `bg_free_inodes_count`.
- **Inode** (in the group's inode table; inode N is in group
  `(N-1)/inodes_per_group`, index `(N-1)%inodes_per_group`, at byte
  `index*s_inode_size`): `i_mode` (type bits: `0x8000` reg, `0x4000` dir),
  `i_size`, `i_links_count`, `i_blocks`, `i_block[15]` (0..11 direct, 12 single
  indirect, 13 double, 14 triple). Root directory is **inode 2**.
- **Directory entry** (`ext2_dir_entry_2`): `inode` (u32), `rec_len` (u16),
  `name_len` (u8), `file_type` (u8), `name[]`. Entries pack a block; `rec_len`
  spans free space. A 0 `inode` with nonzero `rec_len` is a hole.

**Offset → block mapping** (read & write share it): block index `b` of a file →
`i_block[b]` for `b<12`; else single-indirect (`i_block[12]` points to a block of
`256` u32 block numbers, for 1024-byte blocks), then double (`i_block[13]`),
then triple (`i_block[14]`). A helper `bmap(inode, b, alloc?)` returns the
physical block, allocating indirect+data blocks when `alloc` is set.

**Out of scope (YAGNI):** journaling, extents (ext4), xattrs, symlinks (the
sysroot is staged with `cp -RL`, so the image has none), device/fifo/socket
inodes, access-time updates, orphan lists. `file_type` other than reg/dir is
ignored on read.

## Read path

- `ext2_mount`: read the superblock, validate `s_magic == 0xEF53`, cache it +
  the group descriptors; return a vnode for the root (inode 2). On bad magic,
  return NULL (kmain leaves `/disk` unmounted, as it does today when no disk).
- `mkvnode(inum)`: read the inode, set `vnode.type` (dir/file) + `vnode.size`,
  stash `inum` in `priv` (like SFS).
- `ext2_read(vn, off, buf, len)`: clamp to `i_size`; for each block in range,
  `bmap` (no alloc) → `eb_read` → copy the slice; a hole (block 0) reads as
  zeros.
- `ext2_lookup(dir, name)`: walk `dir`'s data blocks, scan dir entries, match
  `name` (by `name_len`), return `mkvnode(entry.inode)`.
- `ext2_readdir(dir, index, name_out)`: return the Nth real entry's name (skip
  holes, skip `.`/`..`? — return them too; callers filter). Used by getdents64.

## Write path

- **Bitmap allocation:** `alloc_block()` / `alloc_inode()` scan the group block/
  inode bitmaps for a free bit, set it, decrement the group + superblock free
  counts, write back the bitmap + counts. `free_block(b)` / `free_inode(n)`
  clear the bit and bump counts.
- `ext2_write(vn, off, buf, len)`: for each block touched, `bmap(..., alloc=1)`
  to get/allocate the physical block (allocating indirect blocks as needed),
  `eb_read`-modify-`eb_write` the slice; grow `i_size`; write the inode back.
- `ext2_truncate(vn)`: free all data + indirect blocks, set `i_size=0`,
  `i_blocks=0`, zero `i_block[]`, write the inode (honors `O_TRUNC` —
  re-saving replaces contents, the bug class we just fixed for ramfs/sfs).
- `ext2_create(dir, name, type)`: `alloc_inode`, init the inode (mode reg/dir,
  links 1, size 0), for a dir allocate a block with `.`/`..`; then insert a
  directory entry into `dir` (find an entry whose `rec_len` has room, split it;
  else allocate a new dir block). Write inode + dir back. Return `mkvnode`.
- `ext2_unlink(dir, name)`: find the entry, merge its `rec_len` into the
  previous entry (or zero its inode), decrement the target's `i_links_count`;
  at 0 links, `ext2_truncate` it and `free_inode`. (Add an `unlink` vnode op +
  `vfs_unlink`.)
- All metadata writes go straight through `block_write` (write-through; no cache
  beyond the in-memory superblock/group-desc, which are re-written on change).

## Mount, persistence, kmain

- `kmain.c`: `if (block present) vfs_mount_at("/disk", ext2_mount())`. The boot
  counter `/disk/boots` and the `/init.l` boot seed move onto ext2 (same code,
  now backed by ext2 via the VFS — no SFS-specific calls remain). PID 1 still
  loads `/init.l`.
- The **fresh disk** is no longer mkfs'd at runtime; it's the host-built ext2
  image (which already contains `/init.l` + the sysroot). A blank/zeroed disk
  fails the magic check and `/disk` stays unmounted (graceful).

## Host image build & musl sysroot

`Makefile` (replacing the `dd` + `mkdisk.py` rules):

```
MUSL_SYSROOT := $(shell aarch64-linux-musl-gcc -print-sysroot)
MKE2FS       := /opt/homebrew/opt/e2fsprogs/sbin/mke2fs
DISK_MB      := 64

build/disk.img: tools/stage-rootfs.sh | $(BUILD)
	rm -rf build/rootfs && mkdir -p build/rootfs/usr/include build/rootfs/usr/lib build/rootfs/test
	printf '(run-bg "lisp" "-frame")\n' > build/rootfs/init.l
	cp -RL $(MUSL_SYSROOT)/include/* build/rootfs/usr/include/
	cp $(MUSL_SYSROOT)/lib/libc.a build/rootfs/usr/lib/
	cp $(MUSL_SYSROOT)/lib/crt1.o $(MUSL_SYSROOT)/lib/crti.o $(MUSL_SYSROOT)/lib/crtn.o build/rootfs/usr/lib/
	# libtcc1.a added here too IF tcc needs its runtime helpers (data-driven)
	# Deterministic KTEST fixtures (known content, known sizes):
	printf 'ext2-small-file-ok\n' > build/rootfs/test/small.txt
	# big.bin: 16 KiB spanning past the 12 direct blocks (forces indirect)
	yes 0123456789ABCDEF | head -c 16384 > build/rootfs/test/big.bin
	dd if=/dev/zero of=$@ bs=1m count=$(DISK_MB) 2>/dev/null
	$(MKE2FS) -F -q -b 1024 -d build/rootfs $@
```
(Staging is a Makefile recipe; the `tools/stage-rootfs.sh` dependency is a thin
script holding the recipe if it grows — inline recipe is fine to start.)

`cp -RL` dereferences symlinks, so the image is symlink-free (driver needn't
handle them). The exact musl lib path is whatever `-print-sysroot` + `/lib`
resolves to; the Makefile uses the variable (verify `crt1.o`/`libc.a` exist
there — they do per `-print-file-name`).

## tcc → real libc

`cc` (system.l) becomes the musl static link line (classic crt ordering: crt1,
crti, the object, libc, crtn):

```lisp
(defun cc (src out)
  (run "tcc" "-nostdlib" "-static" "-Wl,-Ttext=0x8000000000"
       "-I/disk/usr/include"
       "/disk/usr/lib/crt1.o" "/disk/usr/lib/crti.o" src
       "-L/disk/usr/lib" "-lc" "/disk/usr/lib/crtn.o" "-o" out))
```

Default `/hello.c` (initrd) becomes:
```c
#include <stdio.h>
int main(void){ printf("hello from tcc on myosv2: x=%d\n", 42); return 0; }
```
The freestanding path (`mycrt.o` + `puts`) stays available for no-libc programs
(a `cc-bare` helper, or documented manual flags). The runtime is proven by
busybox/mhello; the new risk is tcc's linker emitting a valid static-musl ELF
(init_array/TLS) — addressed data-driven (build, run, fix; add `libtcc1.a` to the
sysroot if tcc references its helpers).

## Testing (TDD)

The disk is now a real ext2 image, so the test harnesses change:
- **`make test`** builds `build/disk.img` via `mke2fs -d` and boots with it. The
  KTESTs (replacing `sfs:`) run in QEMU against that image.
- **`tools/lm_harness.py`** copies `build/disk.img` to its per-PID scratch path
  (instead of truncating a zeroed file), so frame/serial checks mount valid ext2.

**KTESTs** (`src/tests.c`, `ext2:` group), against the booted image, using the
baked fixtures `/disk/test/small.txt` (`"ext2-small-file-ok\n"`) and
`/disk/test/big.bin` (16 KiB of a repeating 16-byte pattern, spanning past the
12 direct blocks):
- `ext2: mount root is dir` — mount, root vnode is a directory.
- `ext2: read small file` — read `/disk/test/small.txt`, exact bytes.
- `ext2: read large file via indirect` — read `/disk/test/big.bin`; the byte at
  offset 13000 (past block 12) matches the known pattern; total size 16384.
- `ext2: create + write + read` — create `/disk/t`, write, read back.
- `ext2: truncate resets` — write 16, truncate, write 3, read back 3.
- `ext2: unlink` — create, unlink, lookup fails.
- `ext2: persists across remount` — write `/disk/t`, drop + re-`ext2_mount`,
  read it back (same booted image).

**Integration check** (`tools/`): `printf` end to end — boot, `(cc "/p.c" "/p")`
where `/p.c` is `#include <stdio.h>\nint main(){printf("x=%d %s\\n",7,"hi");return
0;}`, `(run-file "/p")`, assert the serial/buffer shows `x=7 hi`. Plus a
persistence check: write a file to `/disk` in one boot, read it in a fresh boot
of the same image.

All existing KTESTs unrelated to SFS stay green; the `block:` device tests are
unchanged.

## Risks

- **tcc static-musl link correctness** (init_array/TLS/relocations): the biggest
  unknown. Mitigation: busybox/mhello prove the runtime; iterate on the tcc link
  line; bring in `libtcc1.a` if needed. Fallback if a clean musl link proves too
  hard: keep the freestanding `puts`/`print` path working (already does).
- **ext2 write correctness** (bitmaps, indirect allocation, dir-entry split): the
  intricate part. Mitigation: TDD each op; verify against a Linux/`e2fsck`-clean
  image on the host (`e2fsck -fn build/disk.img` after a test that writes, run on
  the host image post-hoc where feasible).
- **Performance:** reading ~100 headers over synchronous one-sector virtio-blk
  is slow but acceptable; a small block-cache is a future optimization, not now.
- **Disk-full / corruption:** allocation failure → `-ENOSPC`; bad magic/oversize
  fields → mount fails gracefully (no `/disk`).

## Decomposition (one spec, phased plan)

Built and tested in this order, each phase green before the next:
1. **ext2 read** — structs, mount, `bmap` (read), read/lookup/readdir + the read
   KTESTs (against a host-built fixture image).
2. **ext2 write** — bitmaps, alloc/free, `bmap` (alloc), write/grow, truncate,
   create, unlink (+ `vfs_unlink`/`unlink` op) + the write KTESTs.
3. **Cut over `/disk`** — mount ext2 in kmain, move the boot seed/counter onto
   it, delete `src/sfs.c` + `tools/mkdisk.py`, convert `sfs:` KTESTs.
4. **Host image + sysroot** — Makefile `mke2fs -d` build, bake the musl sysroot
   + fixtures, bump to 64 MB, update `lm_harness` to copy the image.
5. **printf** — `cc` musl link line, `#include <stdio.h>` default `hello.c`, the
   printf integration check.
