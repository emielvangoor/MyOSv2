// vfs.c -- path resolution and the generic file API on top of vnode_ops.
// ======================================================================
//
// The VFS knows nothing about how files are stored. It walks paths and calls the
// concrete filesystem's vnode_ops (read/write/lookup/create/readdir). Swapping in
// a new filesystem (e.g. FAT on a disk) means implementing those ops -- nothing
// here changes.

#include <stdint.h>
#include "vfs.h"
#include "kheap.h"
#include "pipe.h"

static struct vnode *root;

void vfs_mount_root(struct fs_type *fs)
{
    root = fs->mount();
}

struct vnode *vfs_root(void)
{
    return root;
}

// Walk "/a/b/c" from the root, one component at a time, via ops->lookup.
struct vnode *vfs_lookup(const char *path)
{
    if (!root || path[0] != '/') {
        return 0;
    }
    if (path[1] == '\0') {
        return root;                 // "/"
    }
    struct vnode *cur = root;
    const char *p = path + 1;
    char name[32];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 31) { name[i++] = *p++; }
        name[i] = '\0';
        if (*p == '/') { p++; }
        if (!cur->ops->lookup) { return 0; }
        cur = cur->ops->lookup(cur, name);
        if (!cur) { return 0; }
    }
    return cur;
}

// Split a path into "parent directory" + "final name", then ask the directory
// to create the entry.
struct vnode *vfs_create(const char *path, int type)
{
    int len = 0;
    while (path[len]) { len++; }
    int slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') { slash = i; }
    }
    if (slash < 0) { return 0; }

    char parent[128];
    if (slash == 0) {                // parent is root: "/x"
        parent[0] = '/'; parent[1] = '\0';
    } else {
        for (int i = 0; i < slash; i++) { parent[i] = path[i]; }
        parent[slash] = '\0';
    }
    char name[32];
    int j = 0;
    for (int i = slash + 1; i < len && j < 31; i++) { name[j++] = path[i]; }
    name[j] = '\0';

    struct vnode *dir = vfs_lookup(parent);
    if (!dir || !dir->ops->create) { return 0; }
    return dir->ops->create(dir, name, type);
}

struct file *vfs_open(const char *path)
{
    struct vnode *vn = vfs_lookup(path);
    if (!vn) { return 0; }
    struct file *f = kmalloc(sizeof(struct file));
    f->vnode = vn;
    f->off = 0;
    f->pipe = 0;
    f->writable = 0;
    f->ref = 1;
    return f;
}

int vfs_read(struct file *f, void *buf, uint64_t len)
{
    if (f->pipe) { return pipe_read(f, buf, len); }    // pipe end
    if (!f->vnode->ops->read) { return -1; }
    int n = f->vnode->ops->read(f->vnode, f->off, buf, len);
    if (n > 0) { f->off += (uint64_t)n; }
    return n;
}

int vfs_write(struct file *f, const void *buf, uint64_t len)
{
    if (f->pipe) { return pipe_write(f, buf, len); }   // pipe end
    if (!f->vnode->ops->write) { return -1; }
    int n = f->vnode->ops->write(f->vnode, f->off, buf, len);
    if (n > 0) { f->off += (uint64_t)n; }
    return n;
}

int vfs_readdir(struct vnode *dir, int index, char *name_out)
{
    if (!dir->ops->readdir) { return -1; }
    return dir->ops->readdir(dir, index, name_out);
}

void vfs_close(struct file *f)
{
    if (--f->ref > 0) { return; }     // other fds still reference this file
    if (f->pipe) { pipe_close(f); }   // drop our end of the pipe
    kfree(f);
}

// Bump a file's reference count (used by dup2 and fork to share a handle).
struct file *file_dup(struct file *f)
{
    f->ref++;
    return f;
}
