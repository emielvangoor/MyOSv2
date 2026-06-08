# Phase 7 notes — VFS + ramfs + initrd

## What changed

MyOSv2 has a filesystem. A **Virtual File System (VFS)** layer gives the kernel
one uniform way to read/write files; an in-memory **ramfs** sits under it; an
embedded **initrd** is unpacked into ramfs at boot. Observed:

```
fs: /hello.txt = "Hello, files!"     (from the initrd)
fs: /tmp.txt   = "written at runtime"
fs: ls / -> tmp.txt motd hello.txt
```

## The VFS abstraction (why it's pluggable)

The whole point: nothing above the VFS knows *which* filesystem stores a file.

- `struct vnode` = a file or directory. It carries a pointer to a
  `struct vnode_ops` vtable (read / write / lookup / create / readdir) and an
  opaque `priv` pointer for filesystem-private data.
- `struct fs_type` = a filesystem driver: a name + a `mount()` that returns a root
  vnode.
- `struct file` = an open handle: a vnode + a byte offset.

A concrete filesystem implements the `vnode_ops` vtable and a `mount`. To add
FAT-on-disk later: implement those same ops over a block device and call
`vfs_mount_root` — `vfs.c`, the syscalls, `exec`, and the shell don't change. That
is the value of the abstraction.

## Path resolution

`vfs_lookup("/a/b/c")` starts at the root vnode and calls `ops->lookup` once per
path component, walking down the tree. `vfs_create` splits a path into "parent
directory + final name", looks up the parent, and calls its `ops->create`.

## ramfs

Each `ramfs_node` embeds a `vnode` (with `vnode.priv` pointing back to the node).
Directories hold a linked list of children; files hold a growable `kmalloc`
buffer. `write` reallocates the buffer when it needs to grow and updates the size;
`read` copies out of it; `lookup`/`readdir` walk the children. Simplest possible
filesystem, but it exercises the entire VFS interface.

## initrd

A C table of `{path, bytes, len}` compiled into the kernel image, unpacked into
the mounted filesystem at boot with `vfs_create` + `vfs_write`. No external build
tool. Phase 8 adds a *program binary* to this table so `exec` has something to
load.

## Testing

Nine deterministic, in-kernel tests (no EL0): mount-is-dir, create+lookup,
write+read, read-at-offset, write-grows-file, readdir-lists, lookup-missing-null,
nested-directory, initrd-unpacked. Written test-first.

## Known limits (future work)

No permissions / owners / timestamps / links; the growable file buffer leaks its
old allocation on regrow; a single mount point (no mount table yet); no buffer
cache; directory order is reverse-insertion. A real on-disk filesystem + a
virtio-block driver is the natural next filesystem to add under this VFS.
