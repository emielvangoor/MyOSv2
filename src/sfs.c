// sfs.c -- a simple persistent inode filesystem (SFS) on the block device.
// ========================================================================
//
// Layout (512-byte blocks): block 0 = superblock; blocks 1..8 = inode table
// (8 inodes/block, 64 total); blocks 9.. = data. Inode 1 is the root directory.
// Allocation is bump-only (no delete) -- minimal but real, and it persists,
// because every change is written through to the disk.

#include <stdint.h>
#include "sfs.h"
#include "vfs.h"
#include "block.h"
#include "kheap.h"

#define SFS_MAGIC   0x53465331u   // "SFS1"
#define INODE_START 1
#define INODE_BLOCKS 8
#define DATA_START  9
#define NDIRECT     12

struct sfs_super {
    uint32_t magic, ninodes, inode_start, inode_blocks;
    uint32_t data_start, nblocks, next_inode, next_block;
};

struct sfs_inode {
    uint32_t type;          // 0 free, 1 file, 2 dir
    uint32_t size;
    uint32_t direct[NDIRECT];
    uint32_t pad[2];        // -> 64 bytes
};

struct sfs_dirent { uint32_t inode; char name[28]; };   // 32 bytes, 16 per block

static struct sfs_super super;          // cached superblock
static const uint8_t zeroblk[512];      // a zeroed sector (static => no memset)

static void wr_super(void) { block_write(0, &super); }

// --- block helpers (each read/write is a full 512-byte sector) ---

static void rd_block(uint32_t b, void *buf) { block_read(b, buf); }
static void wr_block(uint32_t b, const void *buf) { block_write(b, buf); }

// Read/write inode N (lives at byte (N%8)*64 of block inode_start + N/8).
static void rd_inode(uint32_t n, struct sfs_inode *out)
{
    uint8_t blk[512];
    rd_block(super.inode_start + n / 8, blk);
    const uint8_t *src = blk + (n % 8) * 64;
    uint8_t *dst = (uint8_t *)out;
    for (int i = 0; i < 64; i++) { dst[i] = src[i]; }
}
static void wr_inode(uint32_t n, const struct sfs_inode *in)
{
    uint8_t blk[512];
    rd_block(super.inode_start + n / 8, blk);
    uint8_t *dst = blk + (n % 8) * 64;
    const uint8_t *src = (const uint8_t *)in;
    for (int i = 0; i < 64; i++) { dst[i] = src[i]; }
    wr_block(super.inode_start + n / 8, blk);
}

static uint32_t alloc_inode(void) { uint32_t n = super.next_inode++; wr_super(); return n; }
static uint32_t alloc_block(void) { uint32_t b = super.next_block++; wr_super(); return b; }

// --- vnodes (priv holds the inode number) ---

extern const struct vnode_ops sfs_ops;

static struct vnode *mkvnode(uint32_t inum)
{
    struct sfs_inode ino;
    rd_inode(inum, &ino);
    struct vnode *vn = kmalloc(sizeof(struct vnode));
    vn->type = (ino.type == 2) ? VN_DIR : VN_FILE;
    vn->size = ino.size;
    vn->ops = &sfs_ops;
    vn->priv = (void *)(uintptr_t)inum;
    return vn;
}

static uint32_t inum_of(struct vnode *vn) { return (uint32_t)(uintptr_t)vn->priv; }

// --- file read/write over the inode's direct blocks ---

static int sfs_read(struct vnode *vn, uint64_t off, void *buf, uint64_t len)
{
    struct sfs_inode ino;
    rd_inode(inum_of(vn), &ino);
    if (off >= ino.size) { return 0; }
    if (off + len > ino.size) { len = ino.size - off; }

    uint8_t *out = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bi = (uint32_t)(pos / 512);
        uint32_t boff = (uint32_t)(pos % 512);
        if (bi >= NDIRECT || ino.direct[bi] == 0) { break; }
        uint8_t blk[512];
        rd_block(ino.direct[bi], blk);
        uint64_t n = 512 - boff;
        if (n > len - done) { n = len - done; }
        for (uint64_t i = 0; i < n; i++) { out[done + i] = blk[boff + i]; }
        done += n;
    }
    return (int)done;
}

static int sfs_write(struct vnode *vn, uint64_t off, const void *buf, uint64_t len)
{
    uint32_t inum = inum_of(vn);
    struct sfs_inode ino;
    rd_inode(inum, &ino);

    const uint8_t *in = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bi = (uint32_t)(pos / 512);
        uint32_t boff = (uint32_t)(pos % 512);
        if (bi >= NDIRECT) { break; }                    // file size capped at 6 KiB
        if (ino.direct[bi] == 0) { ino.direct[bi] = alloc_block(); }
        uint8_t blk[512];
        rd_block(ino.direct[bi], blk);
        uint64_t n = 512 - boff;
        if (n > len - done) { n = len - done; }
        for (uint64_t i = 0; i < n; i++) { blk[boff + i] = in[done + i]; }
        wr_block(ino.direct[bi], blk);
        done += n;
    }
    if (off + done > ino.size) { ino.size = (uint32_t)(off + done); }
    wr_inode(inum, &ino);
    vn->size = ino.size;
    return (int)done;
}

// --- directory ops (one data block of dirents) ---

static struct vnode *sfs_lookup(struct vnode *dir, const char *name)
{
    struct sfs_inode dino;
    rd_inode(inum_of(dir), &dino);
    if (dino.direct[0] == 0) { return 0; }
    uint8_t blk[512];
    rd_block(dino.direct[0], blk);
    struct sfs_dirent *de = (struct sfs_dirent *)blk;
    for (int i = 0; i < 16; i++) {
        if (de[i].inode == 0) { continue; }
        int eq = 1;
        for (int j = 0; j < 28; j++) {
            char a = de[i].name[j], b = name[j];
            if (a != b) { eq = 0; break; }
            if (a == 0) { break; }
        }
        if (eq) { return mkvnode(de[i].inode); }
    }
    return 0;
}

static struct vnode *sfs_create(struct vnode *dir, const char *name, int type)
{
    uint32_t dnum = inum_of(dir);
    struct sfs_inode dino;
    rd_inode(dnum, &dino);
    if (dino.direct[0] == 0) { dino.direct[0] = alloc_block(); wr_block(dino.direct[0], zeroblk); wr_inode(dnum, &dino); }

    // Create the new inode.
    uint32_t inum = alloc_inode();
    struct sfs_inode ino;
    for (unsigned i = 0; i < sizeof(ino) / 4; i++) { ((uint32_t *)&ino)[i] = 0; }
    ino.type = (type == VN_DIR) ? 2 : 1;
    if (type == VN_DIR) { ino.direct[0] = alloc_block(); wr_block(ino.direct[0], zeroblk); }
    wr_inode(inum, &ino);

    // Add a directory entry.
    uint8_t blk[512];
    rd_block(dino.direct[0], blk);
    struct sfs_dirent *de = (struct sfs_dirent *)blk;
    for (int i = 0; i < 16; i++) {
        if (de[i].inode != 0) { continue; }
        de[i].inode = inum;
        int j = 0;
        while (name[j] && j < 27) { de[i].name[j] = name[j]; j++; }
        de[i].name[j] = 0;
        wr_block(dino.direct[0], blk);
        return mkvnode(inum);
    }
    return 0;   // directory full
}

static int sfs_readdir(struct vnode *dir, int index, char *name_out)
{
    struct sfs_inode dino;
    rd_inode(inum_of(dir), &dino);
    if (dino.direct[0] == 0) { return -1; }
    uint8_t blk[512];
    rd_block(dino.direct[0], blk);
    struct sfs_dirent *de = (struct sfs_dirent *)blk;
    int seen = 0;
    for (int i = 0; i < 16; i++) {
        if (de[i].inode == 0) { continue; }
        if (seen == index) {
            int j = 0;
            while (de[i].name[j] && j < 31) { name_out[j] = de[i].name[j]; j++; }
            name_out[j] = 0;
            return 0;
        }
        seen++;
    }
    return -1;
}

const struct vnode_ops sfs_ops = {
    .read = sfs_read, .write = sfs_write,
    .lookup = sfs_lookup, .create = sfs_create, .readdir = sfs_readdir,
};

// --- mkfs / mount ---

void sfs_mkfs(void)
{
    super.magic = SFS_MAGIC;
    super.ninodes = INODE_BLOCKS * 8;
    super.inode_start = INODE_START;
    super.inode_blocks = INODE_BLOCKS;
    super.data_start = DATA_START;
    super.nblocks = 8192;            // 4 MiB / 512
    super.next_inode = 2;            // inode 1 = root
    super.next_block = DATA_START + 1;
    wr_super();

    // Zero the inode table.
    for (uint32_t b = 0; b < INODE_BLOCKS; b++) { wr_block(INODE_START + b, zeroblk); }

    // Root inode (inode 1): an empty directory with one data block (block 9).
    struct sfs_inode root;
    for (unsigned i = 0; i < sizeof(root) / 4; i++) { ((uint32_t *)&root)[i] = 0; }
    root.type = 2;
    root.direct[0] = DATA_START;
    wr_inode(1, &root);
    wr_block(DATA_START, zeroblk);
}

struct vnode *sfs_mount(void)
{
    block_read(0, &super);
    if (super.magic != SFS_MAGIC) {     // blank disk -> format it
        sfs_mkfs();
        block_read(0, &super);
    }
    return mkvnode(1);                   // the root directory
}

static struct fs_type sfs_fs = { .name = "sfs", .mount = sfs_mount };
struct fs_type *sfs_type(void) { return &sfs_fs; }
