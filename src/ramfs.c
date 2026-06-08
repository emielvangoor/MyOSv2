// ramfs.c -- an in-memory filesystem: files and directories live in kmalloc'd
// nodes. Implements the VFS vnode_ops vtable.
// ============================================================================
//
// Each ramfs_node embeds a `struct vnode` (with vnode.priv pointing back to the
// node). Directories keep a linked list of child nodes; files keep a growable
// byte buffer. This is the simplest possible filesystem -- and it exercises the
// whole VFS interface.

#include <stdint.h>
#include "ramfs.h"
#include "vfs.h"
#include "kheap.h"

struct ramfs_node {
    char name[32];
    int  type;
    uint8_t *data;
    uint64_t size, cap;
    struct vnode vnode;                  // this node's vnode (vnode.priv -> node)
    struct ramfs_node *children, *next;  // directory entries
};

static int rstreq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < 31) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

extern const struct vnode_ops ramfs_ops;   // defined below

static struct ramfs_node *new_node(const char *name, int type)
{
    struct ramfs_node *n = kmalloc(sizeof(struct ramfs_node));
    copy_name(n->name, name);
    n->type = type;
    n->data = 0; n->size = 0; n->cap = 0;
    n->children = 0; n->next = 0;
    n->vnode.type = type;
    n->vnode.size = 0;
    n->vnode.ops = &ramfs_ops;
    n->vnode.priv = n;
    return n;
}

static struct vnode *ramfs_lookup(struct vnode *dir, const char *name)
{
    struct ramfs_node *d = dir->priv;
    for (struct ramfs_node *c = d->children; c; c = c->next) {
        if (rstreq(c->name, name)) { return &c->vnode; }
    }
    return 0;
}

static struct vnode *ramfs_create(struct vnode *dir, const char *name, int type)
{
    struct ramfs_node *d = dir->priv;
    struct ramfs_node *n = new_node(name, type);
    n->next = d->children;            // prepend to the children list
    d->children = n;
    return &n->vnode;
}

static int ramfs_read(struct vnode *vn, uint64_t off, void *buf, uint64_t len)
{
    struct ramfs_node *n = vn->priv;
    if (off >= n->size) { return 0; }
    uint64_t avail = n->size - off;
    if (len > avail) { len = avail; }
    uint8_t *b = buf;
    for (uint64_t i = 0; i < len; i++) { b[i] = n->data[off + i]; }
    return (int)len;
}

static int ramfs_write(struct vnode *vn, uint64_t off, const void *buf, uint64_t len)
{
    struct ramfs_node *n = vn->priv;
    uint64_t end = off + len;
    if (end > n->cap) {                  // grow the buffer
        uint64_t newcap = end < 64 ? 64 : end;
        uint8_t *nd = kmalloc(newcap);
        for (uint64_t i = 0; i < n->size; i++) { nd[i] = n->data[i]; }
        n->data = nd;                    // old buffer leaks (accepted)
        n->cap = newcap;
    }
    const uint8_t *b = buf;
    for (uint64_t i = 0; i < len; i++) { n->data[off + i] = b[i]; }
    if (end > n->size) {
        n->size = end;
        n->vnode.size = end;
    }
    return (int)len;
}

static int ramfs_readdir(struct vnode *dir, int index, char *name_out)
{
    struct ramfs_node *d = dir->priv;
    int i = 0;
    for (struct ramfs_node *c = d->children; c; c = c->next) {
        if (i == index) { copy_name(name_out, c->name); return 0; }
        i++;
    }
    return -1;
}

const struct vnode_ops ramfs_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .readdir = ramfs_readdir,
};

static struct vnode *ramfs_mount(void)
{
    struct ramfs_node *r = new_node("/", VN_DIR);
    return &r->vnode;
}

static struct fs_type ramfs_fs = { .name = "ramfs", .mount = ramfs_mount };

struct fs_type *ramfs_type(void)
{
    return &ramfs_fs;
}
