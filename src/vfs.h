// vfs.h -- the Virtual File System: one interface over any filesystem.
#pragma once
#include <stdint.h>

enum vnode_type { VN_FILE = 0, VN_DIR = 1 };

struct vnode;

// The operations a concrete filesystem implements (a vtable per vnode).
struct vnode_ops {
    int (*read)(struct vnode *vn, uint64_t off, void *buf, uint64_t len);
    int (*write)(struct vnode *vn, uint64_t off, const void *buf, uint64_t len);
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    struct vnode *(*create)(struct vnode *dir, const char *name, int type);
    int (*readdir)(struct vnode *dir, int index, char *name_out); // 0=ok, -1=done
};

struct vnode {
    int type;                     // VN_FILE or VN_DIR
    uint64_t size;
    const struct vnode_ops *ops;
    void *priv;                   // filesystem-private data
};

struct fs_type { const char *name; struct vnode *(*mount)(void); }; // mount -> root
struct file    { struct vnode *vnode; uint64_t off; };             // an open handle

void          vfs_mount_root(struct fs_type *fs);
struct vnode *vfs_root(void);
struct vnode *vfs_lookup(const char *path);
struct vnode *vfs_create(const char *path, int type);
struct file  *vfs_open(const char *path);
int           vfs_read(struct file *f, void *buf, uint64_t len);
int           vfs_write(struct file *f, const void *buf, uint64_t len);
int           vfs_readdir(struct vnode *dir, int index, char *name_out);
void          vfs_close(struct file *f);
