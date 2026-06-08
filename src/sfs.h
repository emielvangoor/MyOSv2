// sfs.h -- a simple persistent inode filesystem on the block device.
// Implements the VFS vnode_ops, so files live on the virtio-blk disk and
// survive a reboot.
#pragma once
#include "vfs.h"

void           sfs_mkfs(void);    // format the disk (superblock + empty root)
struct vnode  *sfs_mount(void);   // read the superblock (mkfs if blank) -> root vnode
struct fs_type *sfs_type(void);   // for vfs_mount_root, if ever used as the root
