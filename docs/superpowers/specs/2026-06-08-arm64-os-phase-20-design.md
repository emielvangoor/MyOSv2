# MyOSv2 — Phase 20 Design (on-disk filesystem)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

A **persistent** filesystem on the virtio-blk disk, mounted under the VFS, so
files survive a reboot — the payoff for Phase 19. We implement a small custom
**inode filesystem (SFS)** as a VFS `fs_type` backed by `block_read`/`block_write`,
and add minimal **mount-point** support so it lives at `/disk` alongside the
ramfs root.

Builds on the VFS (7) and the block device (19).

## On-disk format (SFS)

512-byte blocks. Fixed, simple, bump-allocated (no delete/free — out of scope).

```
block 0      superblock
blocks 1..8  inode table (8 blocks x 8 inodes = 64 inodes)
blocks 9..   data blocks
```

**Superblock** (block 0):
```c
struct sfs_super {
    uint32_t magic;        // 0x53465331 "SFS1"
    uint32_t ninodes;      // 64
    uint32_t inode_start;  // 1
    uint32_t inode_blocks; // 8
    uint32_t data_start;   // 9
    uint32_t nblocks;      // disk size / 512
    uint32_t next_inode;   // bump allocator (root = 1, so starts at 2)
    uint32_t next_block;   // bump allocator for data blocks
};
```

**Inode** (64 bytes; 8 per block):
```c
struct sfs_inode {
    uint32_t type;         // 0 free, 1 file, 2 dir
    uint32_t size;         // bytes
    uint32_t direct[12];   // up to 12 data blocks => 6 KiB max file
    uint32_t _pad[2];
};
```
Inode 0 is unused; **inode 1 is the root directory**.

**Directory** data block holds entries (32 bytes; 16 per block):
```c
struct sfs_dirent { uint32_t inode; char name[28]; };   // inode 0 = empty slot
```
(One data block per directory in this minimal version → up to 16 entries.)

## mkfs + mount

- `sfs_mkfs()`: write the superblock, zero the inode table, create the root inode
  (dir, one data block), zero that block. `next_inode = 2`,
  `next_block = data_start + 1`.
- `sfs_mount()` (the `fs_type` hook): read block 0; if `magic` is wrong, `mkfs`
  first (so a blank disk self-formats). Return a vnode for the root inode.
- The superblock is cached in RAM and written back on each allocation.

Inode I/O: inode `N` lives at block `inode_start + N/8`, byte `(N%8)*64`. Read =
read the block, copy 64 bytes; write = read-modify-write the block.

## VFS vnode_ops (`sfs.c`)

Each vnode's `priv` holds its inode number. Ops:
- `read(vn, off, buf, len)`: walk the inode's `direct[]` blocks, copy out, capped
  at `size`.
- `write(vn, off, buf, len)`: allocate `direct[]` blocks on demand, write through
  to the disk, grow `size`, persist the inode.
- `lookup(dir, name)`: scan the directory block's entries; return a fresh vnode
  for the matching inode.
- `create(dir, name, type)`: allocate an inode (and a data block for a dir), add a
  `dirent` to the directory block, return its vnode.
- `readdir(dir, i, name)`: return the i-th non-empty entry's name.

## Mount points (`vfs.c`)

A tiny mount table maps a path prefix to a filesystem root:
```c
void vfs_mount_at(const char *path, struct vnode *root);   // e.g. "/disk"
```
`vfs_lookup` first checks the mounts: if `path` equals or is under a mount prefix,
it resolves the remainder within that mount's root; otherwise it uses the global
ramfs root. `vfs_create` already routes through `vfs_lookup(parent)`, so writes to
`/disk/...` reach SFS automatically.

## kmain integration + persistence demo

After mounting ramfs and probing the disk, `vfs_mount_at("/disk", sfs_mount())`.
Then a boot-counter demo: read `/disk/boots` (a small number), increment it, write
it back, and print `boot count: N`. Re-running `make run` (the disk image
persists) shows N increasing — proof of on-disk persistence.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/sfs.h`/`sfs.c` | on-disk format, mkfs, mount, inode/block I/O, vnode_ops |
| `src/vfs.h`/`vfs.c` | `vfs_mount_at` + mount-aware `vfs_lookup` |
| `src/kmain.c` | mount `/disk`; boot-counter persistence demo |
| `src/tests.c` | SFS tests (test-first) |
| `docs/notes/phase-20.md` | notes |

## Testing (test-first, runs under `make test` on the real disk)

1. `test_sfs_mkfs_mount` — `sfs_mkfs(); v = sfs_mount()`: root is a directory.
2. `test_sfs_create_write_read` — create `/f` under root, write "hello", read it
   back identical; `size == 5`.
3. `test_sfs_readdir` — after creating two files, `readdir` lists both names.
4. `test_sfs_persists_remount` — write a file, **re-mount** (re-read the
   superblock + root from disk, fresh vnodes), look it up, and read the same bytes
   back. (True persistence through the block device.)
5. `test_sfs_multiblock` — write > 512 bytes (spanning `direct[0]`/`direct[1]`),
   read it all back correctly.

## Success criteria

- 5 SFS tests pass under `make test` (test-first); prior tests stay green; gate
  holds.
- Live: `make run` mounts `/disk`, the boot counter increments across runs, and
  `ls /disk` / `cat /disk/<file>` work in the shell.

## Out of scope

Delete/free (truncate, `unlink`, free bitmap); indirect blocks (so files cap at
6 KiB); nested directories beyond one block of entries; permissions; a block
cache; FAT/ext compatibility.
