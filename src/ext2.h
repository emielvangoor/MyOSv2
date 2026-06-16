// ext2.h -- a real on-disk ext2 filesystem on the block device.
// =============================================================
//
// ext2 is the canonical Unix filesystem: inodes with indirect blocks (so files
// can be megabytes, not the 6 KiB SFS capped at), real directories, Unix
// permissions/timestamps. It replaces SFS as the backing store for /disk.
//
// Phase 1 implements the READ path only: mount, inode reads, the block map
// (direct + single/double/triple indirect), file read, directory lookup and
// readdir. The write/create/truncate vnode ops return -1 (read-only) until
// Phase 2 lands. The image is built on the host with `mke2fs -d` (see the
// Makefile), so /disk ships pre-populated with /init.l and the test fixtures.
#pragma once
#include "vfs.h"

struct vnode  *ext2_mount(void);   // read+validate the superblock -> root vnode (or NULL)
struct fs_type *ext2_type(void);   // fs_type registering the name "ext2"
