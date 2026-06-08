# Phase 20 notes — on-disk filesystem (SFS)

## What changed

Files now **persist** on the virtio-blk disk. A small inode filesystem (SFS) is
mounted at `/disk`, and a boot counter proves it survives reboots:

```
disk: /disk mounted (boot count 1)     # first run
disk: /disk mounted (boot count 2)     # next `make run`, same disk image
disk: /disk mounted (boot count 3)
$ ls /disk
boots
$ cat /disk/boots
3
```

## The filesystem (`sfs.c`)

512-byte blocks: block 0 = superblock; blocks 1–8 = inode table (64 inodes,
8/block); blocks 9+ = data. **Inode 1 is the root directory.** Each inode is
64 bytes (`type`, `size`, 12 `direct[]` block pointers → 6 KiB max file). A
directory is one data block of 32-byte entries (`{inode, name}`). Allocation is
**bump-only** (no delete) — minimal, but every change is written straight through
to the disk, so it's real and it persists. `sfs_mount` reads the superblock and
**auto-formats** (mkfs) a blank disk.

Each `struct vnode`'s `priv` just holds its inode number; the `vnode_ops`
(read/write/lookup/create/readdir) read-modify-write the relevant blocks via
`block_read`/`block_write`. Files spanning multiple `direct[]` blocks work
(tested with a 600-byte file).

## Mount points (`vfs.c`)

A tiny mount table maps a path prefix to a filesystem root. `vfs_lookup` checks it
first: a path under `/disk` resolves within SFS; everything else uses the ramfs
root. `vfs_create` routes through `vfs_lookup(parent)`, so writes to `/disk/...`
reach SFS for free.

## Two bugs worth remembering

1. **Stale mount entry.** The mount table is global, and the self-tests'
   `vfs_mount_at("/disk", ...)` left an entry pointing at a vnode that the kheap
   reset (before `kmain`) had recycled. `kmain`'s `vfs_lookup("/disk/boots")` then
   called a **garbage `ops->lookup`** and hung. Fix: `vfs_mount_root` clears the
   mount table (a fresh root ⇒ fresh mounts).
2. **Self-tests wiped the disk.** The suite ran on *every* boot and `mkfs`'d the
   disk, so the boot counter always reset to 1. Fix: run the self-test suite only
   in the `make test` build (`-DTEST_EXIT`); a normal boot goes straight to the OS
   and keeps its data.

## Testing

5 tests, test-first (run under `make test` on the real disk): create/write/read a
file; persistence across a **remount** (fresh vnodes re-read from disk); readdir;
a multi-block file; and the VFS mount-point routing. Cross-reboot persistence is
verified live by the boot counter over successive `make run`s.

## Limits

No delete/truncate/free bitmap; no indirect blocks (6 KiB file cap); one block of
directory entries (≤16 per dir); no permissions or block cache; not FAT/ext
compatible.
