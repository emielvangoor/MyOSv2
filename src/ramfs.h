// ramfs.h -- an in-memory filesystem for the VFS.
#pragma once
#include "vfs.h"

struct fs_type *ramfs_type(void);   // pass to vfs_mount_root()
