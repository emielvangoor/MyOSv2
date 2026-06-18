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
#include "socket.h"

static struct vnode *root;

// A tiny mount table: a path prefix (e.g. "/disk") -> that filesystem's root.
static struct { const char *path; struct vnode *root; } mounts[4];
static int nmounts;

void vfs_mount_root(struct fs_type *fs)
{
    nmounts = 0;          // a fresh root means a fresh mount table
    root = fs->mount();
}

void vfs_mount_at(const char *path, struct vnode *mroot)
{
    if (nmounts < 4) { mounts[nmounts].path = path; mounts[nmounts].root = mroot; nmounts++; }
}

struct vnode *vfs_root(void)
{
    return root;
}

// vfs_readlink: read a symlink's target into buf (up to len bytes). Returns the
// number of bytes written (no NUL terminator), or -1 if the vnode has no
// readlink op (i.e. it is not a symlink, or the filesystem doesn't implement it).
int vfs_readlink(struct vnode *vn, char *buf, int len)
{
    if (!vn->ops->readlink) { return -1; }
    return vn->ops->readlink(vn, buf, len);
}

// Forward declaration so resolve_symlink and walk_from_d can call each other.
static struct vnode *walk_from_d(struct vnode *cur, const char *p, int depth);

// Resolve a symlink vnode `link` whose directory entry lives inside `parent`.
// If the stored target starts with '/', the walk restarts from the VFS root
// (absolute symlink). Otherwise the target is relative and the walk starts from
// `parent` (the directory that contains the symlink). Either way we pass
// `depth+1` into walk_from_d so the hop count is always bounded.
//
// Why depth-bound? A cyclic chain (a -> b -> a) would otherwise loop forever.
// We allow up to 8 hops -- enough for every real busybox applet layout and
// consistent with historical POSIX lore (MAXSYMLINKS == 8 on many systems).
static struct vnode *resolve_symlink(struct vnode *link, struct vnode *parent, int depth)
{
    if (depth >= 8) { return 0; }                 // -ELOOP: too many nested symlinks
    char target[128];
    int n = vfs_readlink(link, target, sizeof(target) - 1);
    if (n <= 0) { return 0; }
    target[n] = '\0';
    if (target[0] == '/') {
        // Absolute target: restart from the global root (mount-unaware, which is
        // acceptable -- we only need this for the rare case of an absolute symlink,
        // and the busybox layout uses relative targets like "busybox").
        if (!root) { return 0; }
        return walk_from_d(root, target + 1, depth + 1);
    }
    // Relative target: continue from the directory that owns the symlink.
    return walk_from_d(parent, target, depth + 1);
}

// Walk the remaining path `p` from `cur`, one component at a time, following any
// symlink we land on. `depth` tracks nested-symlink hops and is forwarded to
// resolve_symlink; the public entry always passes 0 (see walk_from below).
//
// WHY follow here instead of at lookup: the VFS is the only place that knows the
// whole path; the filesystem's lookup op just returns the vnode for a single name.
// Doing the follow here keeps the filesystem code simple and the policy central.
static struct vnode *walk_from_d(struct vnode *cur, const char *p, int depth)
{
    char name[32];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 31) { name[i++] = *p++; }
        name[i] = '\0';
        if (*p == '/') { p++; }
        if (i == 0) { continue; }                 // skip empty components ("//")
        if (!cur->ops->lookup) { return 0; }
        struct vnode *parent = cur;
        struct vnode *next = cur->ops->lookup(cur, name);
        if (!next) { return 0; }
        // If we landed on a symlink, resolve it before continuing. The target may
        // itself be a symlink (depth allows up to 8 total hops).
        if (next->type == VN_SYMLINK) {
            next = resolve_symlink(next, parent, depth);
            if (!next) { return 0; }
        }
        cur = next;
    }
    return cur;
}

// Walk "/a/b/c" from `cur`, following symlinks (the public entry, depth 0).
// Replaces the old symlink-blind version; callers (vfs_lookup) are unchanged.
static struct vnode *walk_from(struct vnode *cur, const char *p)
{
    return walk_from_d(cur, p, 0);
}

// True if `pfx` is a path-prefix of `path` at a '/' boundary (or exact match).
static int under(const char *pfx, const char *path)
{
    int i = 0;
    while (pfx[i]) { if (pfx[i] != path[i]) { return 0; } i++; }
    return path[i] == '\0' || path[i] == '/';
}

// Walk "/a/b/c" from the right filesystem root, one component at a time,
// following any symlinks encountered along the way (up to 8 hops).
struct vnode *vfs_lookup(const char *path)
{
    // A leading '/' marks an absolute path. MyOSv2 has no per-process current
    // directory, so a path *without* one can only mean "relative to the root" --
    // we resolve it straight from `root`. This is what lets the shell accept
    // `cat hello.txt` and `ls bin`, not just their slash-prefixed twins.
    // (Mounts are matched by absolute prefix below, so a relative path never
    // crosses into one -- an acceptable limit while there is no cwd.)
    if (path[0] != '/') {
        if (!root) { return 0; }
        if (path[0] == '\0') { return root; }    // "" == the root directory
        return walk_from(root, path);
    }

    // A mounted filesystem? Resolve the remainder within its root.
    for (int m = 0; m < nmounts; m++) {
        if (under(mounts[m].path, path)) {
            const char *sub = path + 0;
            int n = 0; while (mounts[m].path[n]) { n++; }
            sub = path + n;                  // the part after the mount prefix
            if (*sub == '/') { sub++; }
            if (*sub == '\0') { return mounts[m].root; }   // the mount point itself
            return walk_from(mounts[m].root, sub);
        }
    }

    if (!root) { return 0; }
    if (path[1] == '\0') { return root; }    // "/"
    return walk_from(root, path + 1);
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

// Remove the entry named at the end of `path` from its parent directory. Splits
// the path into "parent" + "final name" exactly like vfs_create, then asks the
// parent's filesystem to unlink the entry. Returns 0 on success, -1 otherwise.
int vfs_unlink(const char *path)
{
    int len = 0;
    while (path[len]) { len++; }
    int slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') { slash = i; }
    }
    if (slash < 0) { return -1; }

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
    if (!dir || !dir->ops->unlink) { return -1; }
    return dir->ops->unlink(dir, name);
}

// Split an absolute PATH into its parent directory path (>=128 bytes) and final
// component NAME (>=64 bytes). Returns 0, or -1 if PATH has no '/'. Mirrors the
// inline split in vfs_create/vfs_unlink, shared by the symlink/rename wrappers.
static int split_parent(const char *path, char *parent, char *name)
{
    int len = 0;
    while (path[len]) { len++; }
    int slash = -1;
    for (int i = 0; i < len; i++) { if (path[i] == '/') { slash = i; } }
    if (slash < 0) { return -1; }
    if (slash == 0) { parent[0] = '/'; parent[1] = '\0'; }
    else { for (int i = 0; i < slash; i++) { parent[i] = path[i]; } parent[slash] = '\0'; }
    int j = 0;
    for (int i = slash + 1; i < len && j < 63; i++) { name[j++] = path[i]; }
    name[j] = '\0';
    return 0;
}

int vfs_symlink(const char *linkpath, const char *target)
{
    char parent[128], name[64];
    if (split_parent(linkpath, parent, name) != 0) { return -1; }
    struct vnode *dir = vfs_lookup(parent);
    if (!dir || !dir->ops->symlink) { return -1; }
    return dir->ops->symlink(dir, name, target);
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    char op[128], on[64], np[128], nn[64];
    if (split_parent(oldpath, op, on) != 0) { return -1; }
    if (split_parent(newpath, np, nn) != 0) { return -1; }
    struct vnode *od = vfs_lookup(op);
    struct vnode *nd = vfs_lookup(np);
    if (!od || !nd || !od->ops->rename) { return -1; }
    return od->ops->rename(od, on, nd, nn);
}

int vfs_link(const char *oldpath, const char *newpath)
{
    struct vnode *target = vfs_lookup(oldpath);   // the existing inode to share
    if (!target) { return -1; }
    char parent[128], name[64];
    if (split_parent(newpath, parent, name) != 0) { return -1; }
    struct vnode *dir = vfs_lookup(parent);
    if (!dir || !dir->ops->link) { return -1; }
    return dir->ops->link(dir, name, target);
}

struct file *vfs_open(const char *path)
{
    struct vnode *vn = vfs_lookup(path);
    if (!vn) { return 0; }
    struct file *f = kmalloc(sizeof(struct file));
    f->vnode = vn;
    f->off = 0;
    f->pipe = 0;
    f->sock = 0;
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

int vfs_truncate(struct vnode *vn)
{
    if (!vn->ops->truncate) { return -1; }
    return vn->ops->truncate(vn);
}

void vfs_close(struct file *f)
{
    if (--f->ref > 0) { return; }     // other fds still reference this file
    if (f->pipe) { pipe_close(f); }   // drop our end of the pipe
    if (f->sock) { socket_free(f->sock); }   // release the socket (see socket.h)
    kfree(f);
}

// Bump a file's reference count (used by dup2 and fork to share a handle).
struct file *file_dup(struct file *f)
{
    f->ref++;
    return f;
}
