// ext2.h -- a real on-disk ext2 filesystem on the block device.
// =============================================================
//
// ext2 is the canonical Unix filesystem: inodes with indirect blocks (so files
// can be megabytes, not the 6 KiB SFS capped at), real directories, Unix
// permissions/timestamps. It is the backing store for the ROOT filesystem `/`
// (it replaced the old SFS, and is now mounted AS / rather than at /disk).
//
// Full read AND write: mount, inode read/write, the block map (direct +
// single/double/triple indirect), file read, directory lookup/readdir, plus
// bitmap allocation, create/write/grow, truncate, and unlink. The image is
// built on the host with `mke2fs -d` (see the Makefile), so the root / ships
// pre-populated with the userland (/bin, /lib, /usr), /init.l, and the
// test fixtures.
#pragma once
#include "vfs.h"

struct vnode  *ext2_mount(void);   // read+validate the superblock -> root vnode (or NULL)
struct fs_type *ext2_type(void);   // fs_type registering the name "ext2"

// Phase 2 (write path): remove a directory entry `name` from `dir`, decrement
// the target inode's link count, and free it (truncate + free_inode) when the
// count hits zero. Returns 0 on success, -1 if not found. Exposed for vfs_unlink.
int ext2_unlink(struct vnode *dir, const char *name);
