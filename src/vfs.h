// vfs.h -- the Virtual File System: one interface over any filesystem.
#pragma once
#include <stdint.h>

enum vnode_type { VN_FILE = 0, VN_DIR = 1, VN_SYMLINK = 2 };

struct vnode;

// The operations a concrete filesystem implements (a vtable per vnode).
struct vnode_ops {
    int (*read)(struct vnode *vn, uint64_t off, void *buf, uint64_t len);
    int (*write)(struct vnode *vn, uint64_t off, const void *buf, uint64_t len);
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    struct vnode *(*create)(struct vnode *dir, const char *name, int type);
    int (*readdir)(struct vnode *dir, int index, char *name_out); // 0=ok, -1=done
    int (*truncate)(struct vnode *vn);          // shrink to 0 bytes (O_TRUNC)
    int (*unlink)(struct vnode *dir, const char *name); // remove an entry from dir
    int (*readlink)(struct vnode *vn, char *buf, int len);  // read a symlink's target
    int (*symlink)(struct vnode *dir, const char *name, const char *target); // make a symlink
    int (*rename)(struct vnode *odir, const char *oldname,
                  struct vnode *ndir, const char *newname);  // move/rename an entry
    int (*link)(struct vnode *dir, const char *name, struct vnode *target); // hard link
};

struct vnode {
    int type;                     // VN_FILE or VN_DIR
    uint64_t size;
    const struct vnode_ops *ops;
    void *priv;                   // filesystem-private data
};

struct fs_type { const char *name; struct vnode *(*mount)(void); }; // mount -> root

struct pipe;   // from pipe.h (a file may be a pipe end instead of a vnode)
struct socket;               // from socket.h

struct file {
    struct vnode  *vnode;    // the file's vnode (NULL for a pipe/socket end)
    uint64_t       off;      // current read/write offset
    struct pipe   *pipe;     // non-NULL => this handle is a pipe end
    struct socket *sock;     // non-NULL => this handle is a socket
    int            writable; // pipe direction: 1 = write end, 0 = read end
    int            ref;      // open references (shared across fork/dup)
};

void          vfs_mount_root(struct fs_type *fs);
void          vfs_mount_at(const char *path, struct vnode *root); // mount a FS at a subdir
struct vnode *vfs_root(void);
struct vnode *vfs_lookup(const char *path);
struct vnode *vfs_create(const char *path, int type);
struct file  *vfs_open(const char *path);
int           vfs_read(struct file *f, void *buf, uint64_t len);
int           vfs_write(struct file *f, const void *buf, uint64_t len);
int           vfs_readdir(struct vnode *dir, int index, char *name_out);
int           vfs_truncate(struct vnode *vn);   // O_TRUNC: reset the file to 0 bytes
int           vfs_unlink(const char *path);     // remove a file/empty entry by path
int           vfs_readlink(struct vnode *vn, char *buf, int len); // copy a symlink target into buf
int           vfs_symlink(const char *linkpath, const char *target); // create symlink at linkpath
int           vfs_rename(const char *oldpath, const char *newpath);  // move/rename oldpath -> newpath
int           vfs_link(const char *oldpath, const char *newpath);    // hard-link newpath -> oldpath's inode
void          vfs_close(struct file *f);
struct file  *file_dup(struct file *f);   // bump the reference count; returns f
