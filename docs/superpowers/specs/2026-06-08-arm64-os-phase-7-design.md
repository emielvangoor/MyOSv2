# MyOSv2 — Phase 7 Design (VFS + ramfs + initrd)

**Date:** 2026-06-08
**Status:** Approved

## Goal

Add a **Virtual File System (VFS)** layer so the kernel reads/writes files through
one uniform interface, independent of the backing filesystem. Implement an
in-memory **ramfs** under it, and an embedded **initrd** unpacked into ramfs at
boot. Designed so a real on-disk filesystem (FAT, ext2) is a clean later add.

First of the 7→11 sequence (VFS → exec → fork/COW → shell → ASIDs). Builds on the
kernel heap (Phase 4).

## VFS architecture (`vfs.h` / `vfs.c`)

Concrete filesystems plug in via function-pointer vtables (the Unix VFS model):

```c
enum vnode_type { VN_FILE = 0, VN_DIR = 1 };

struct vnode {
    int type;
    uint64_t size;
    const struct vnode_ops *ops;   // filled in by the concrete FS
    void *priv;                    // FS-private data
};

struct vnode_ops {
    int (*read)(struct vnode*, uint64_t off, void *buf, uint64_t len);
    int (*write)(struct vnode*, uint64_t off, const void *buf, uint64_t len);
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    struct vnode *(*create)(struct vnode *dir, const char *name, int type);
    int (*readdir)(struct vnode *dir, int index, char *name_out);  // 0 ok, -1 done
};

struct fs_type { const char *name; struct vnode *(*mount)(void); };  // returns root
struct file    { struct vnode *vnode; uint64_t off; };               // open handle
```

VFS API (kernel-internal; user file syscalls come in Phase 8):
- `vfs_mount_root(struct fs_type*)` — mount a filesystem as `/`.
- `vfs_root(void)` — the root vnode.
- `vfs_lookup(path)` — resolve `/a/b/c` by walking `ops->lookup` from the root.
- `vfs_create(path, type)` — split into parent dir + name; `ops->create`.
- `vfs_open(path) → file*`, `vfs_read/vfs_write(file,…)` (advance `off`),
  `vfs_readdir(dir, idx, name)`, `vfs_close`.

Adding a new filesystem = implement the `vnode_ops` vtable + a `mount` and call
`vfs_mount_root`. Nothing above the VFS changes.

## ramfs (`ramfs.h` / `ramfs.c`)

In-memory filesystem implementing the vtable:

```c
struct ramfs_node {
    char name[32];
    int  type;
    uint8_t *data; uint64_t size, cap;   // file bytes (kmalloc; grows on write)
    struct vnode vnode;                  // this node's vnode (priv -> the node)
    struct ramfs_node *children, *next;  // directory entries (linked list)
};
```

- `mount` → a root directory node.
- `lookup`/`readdir` walk the children list; `create` prepends a child.
- `read` copies from `data`; `write` grows `data` (reallocating) and updates size.

`ramfs_type()` returns the `fs_type`.

## initrd (`initrd.h` / `initrd.c`)

An embedded table of files (no external build tool), unpacked into the mounted FS
at boot:

```c
struct initrd_file { const char *path; const char *data; uint64_t len; };
// e.g. { "/hello.txt", "Hello, files!\n", 14 }, { "/motd", ... }
void initrd_unpack(void);   // vfs_create + vfs_write each file
```

Paths are flat (no nested dirs needed). Phase 8 will add a *program binary* to the
initrd for `exec` (which needs a small embed step, handled there).

## Demo (`kmain`)

Mount ramfs, `initrd_unpack()`, then: open `/hello.txt` and print its contents;
`vfs_create("/tmp.txt")`, write a string, re-open and read it back, print it;
`vfs_readdir("/")` and list the files. Visible proof the filesystem works.

## Testing (test-first, ~9 deterministic cases, in-kernel)

Each test mounts a fresh ramfs first. No EL0 needed.

1. mount → root vnode is a directory.
2. create a file, `vfs_lookup` finds it (type FILE).
3. write bytes, re-open, read them back equal.
4. read at an offset returns the right slice.
5. writing past the end grows the file (size updated; read-back equal).
6. `readdir` lists all created entries (order-independent count + names present).
7. lookup of a missing name → NULL.
8. nested dir: create `/d` (dir) + `/d/f.txt` (file); `vfs_lookup("/d/f.txt")` found.
9. after `initrd_unpack`, `/hello.txt` reads back its expected contents.

## Files

| File | Responsibility |
|------|----------------|
| `src/vfs.h` / `vfs.c` | VFS types, path resolution, `vfs_*` API |
| `src/ramfs.h` / `ramfs.c` | in-memory filesystem (vnode ops) |
| `src/initrd.h` / `initrd.c` | embedded file table + unpack |
| `src/tests.c` | ~9 VFS/ramfs tests (test-first) |
| `src/kmain.c` | mount + unpack + filesystem demo |
| `docs/notes/phase-7.md` | notes |

All new code heavily commented.

## Success criteria

- ~9 new `vfs:` tests pass (after RED→GREEN); the demo reads `/hello.txt`,
  writes+reads a new file, and lists `/`; all prior 23 tests stay green; `make
  test` exit 0; gate intact.

## Out of scope (later phases)

File syscalls (open/read/write/close) + `exec` (Phase 8); `fork`/COW (9); the
shell (10); ASIDs (11); a block driver + on-disk FS (a future add — the VFS is
built for it); permissions, links, timestamps, a buffer cache.
