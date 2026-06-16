// ext2.c -- the ext2 read driver, behind the VFS vnode_ops.
// =========================================================
//
// This is the sole consumer of block_read/block_write for /disk. It speaks the
// ext2 on-disk format directly: a superblock describing the geometry, a table
// of block-group descriptors, per-group inode tables, and inodes whose i_block[]
// array maps file offsets to physical blocks (direct + indirect).
//
// WHY ext2 and not something homegrown: ext2 is exhaustively documented and --
// decisively -- the host's e2fsprogs can BUILD and pre-populate an image
// (`mke2fs -d build/rootfs`), so /disk arrives already containing /init.l and
// the KTEST fixtures. The kernel only has to READ what the host laid down.
//
// Block size is 1024 bytes (forced with `mke2fs -b 1024`); the block device
// speaks 512-byte sectors, so one ext2 block = 2 sectors. eb_read/eb_write do
// that 2-sector transfer.
//
// Phase 1 = READ ONLY. write/create/truncate return -1; allocation (bitmaps,
// bmap-with-alloc) is Phase 2.

#include <stdint.h>
#include "ext2.h"
#include "vfs.h"
#include "block.h"
#include "kheap.h"

#define EXT2_MAGIC      0xEF53      // s_magic of a valid ext2 superblock
#define EXT2_BLOCK_SIZE 1024        // we force -b 1024; one block = 2 sectors
#define EXT2_ROOT_INO   2           // inode 2 is always the root directory
#define EXT2_NDIR       12          // i_block[0..11] are direct block pointers
#define EXT2_IND        12          // i_block[12] -> single-indirect block
#define EXT2_DIND       13          // i_block[13] -> double-indirect block
#define EXT2_TIND       14          // i_block[14] -> triple-indirect block
#define PTRS_PER_BLOCK  (EXT2_BLOCK_SIZE / 4)   // 256 u32 block numbers per block

// i_mode top bits give the file type (the low bits are permissions).
#define EXT2_S_IFMT     0xF000
#define EXT2_S_IFREG    0x8000      // regular file
#define EXT2_S_IFDIR    0x4000      // directory

// --- on-disk structures (little-endian; the host is too, so no byte-swapping) ---

// The superblock (at byte offset 1024 == block 1 for 1024-byte blocks). Only the
// fields the read path needs are named; the rest is reserved padding to 1024.
struct ext2_super {
    uint32_t s_inodes_count;        // total inodes in the filesystem
    uint32_t s_blocks_count;        // total blocks
    uint32_t s_r_blocks_count;      // blocks reserved for the superuser
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;    // block holding the superblock (1 at 1024B)
    uint32_t s_log_block_size;      // block size = 1024 << this (0 => 1024)
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;    // inodes per block group (table sizing)
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;               // 0xEF53 for ext2
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;           // 0 = old (128-byte inodes), 1 = dynamic
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // -- the following exist only when s_rev_level >= 1 (EXT2_DYNAMIC_REV) --
    uint32_t s_first_ino;           // first non-reserved inode
    uint16_t s_inode_size;          // size of each on-disk inode (128 or 256)
    uint8_t  s_reserved[934];       // pad to a full 1024-byte block
} __attribute__((packed));

// One block-group descriptor (32 bytes). The descriptor table starts in the
// block right after the superblock and has one entry per group.
struct ext2_group_desc {
    uint32_t bg_block_bitmap;       // block holding this group's block bitmap
    uint32_t bg_inode_bitmap;       // block holding this group's inode bitmap
    uint32_t bg_inode_table;        // first block of this group's inode table
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];       // pad to 32 bytes
} __attribute__((packed));

// An on-disk inode. ext2 inodes are 128 bytes in the old rev and commonly 256
// in the dynamic rev; we only read the first 128 bytes (everything we use lives
// there) and skip the rest via s_inode_size when indexing the table.
struct ext2_inode {
    uint16_t i_mode;                // type bits + permissions
    uint16_t i_uid;
    uint32_t i_size;                // file length in bytes (low 32 bits)
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;              // 512-byte sectors allocated (NOT fs blocks)
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];           // 0..11 direct, 12 single, 13 double, 14 triple
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;             // (high 32 bits of size for large reg files)
    uint32_t i_faddr;
    uint8_t  i_osd2[12];            // -> 128 bytes total
} __attribute__((packed));

// A directory entry (the "_2" linked-list variant: name_len is one byte and the
// freed top byte became file_type). Entries pack into a block back to back;
// rec_len is the distance to the next entry, so the last one's rec_len spans to
// the block end. inode==0 marks a deleted/unused slot (a hole).
struct ext2_dir_entry {
    uint32_t inode;                 // inode number (0 => unused slot)
    uint16_t rec_len;               // bytes to the next entry in this block
    uint8_t  name_len;              // length of name (not counting any padding)
    uint8_t  file_type;             // 1=reg 2=dir ... (ignored on read)
    char     name[];                // name_len bytes, not NUL-terminated
} __attribute__((packed));

// --- cached metadata (filled in by ext2_mount) ---

static struct ext2_super sb;            // the superblock
static struct ext2_group_desc *gdt;     // the block-group descriptor table (kmalloc'd)
static uint32_t ngroups;                // number of block groups
static uint32_t inode_size;             // s_inode_size (128 default)

// --- block helpers: one ext2 block = 2 disk sectors ---

// Read ext2 block `b` (1024 bytes) into buf via two 512-byte sector reads.
static int eb_read(uint32_t b, void *buf)
{
    uint8_t *p = buf;
    if (block_read((uint64_t)b * 2 + 0, p) != 0) { return -1; }
    if (block_read((uint64_t)b * 2 + 1, p + 512) != 0) { return -1; }
    return 0;
}

// Write ext2 block `b` as two sector writes. Unused in Phase 1 (read-only) but
// defined so Phase 2 can drop in the write path without touching this file's
// block layer. Marked unused to keep -Wall -Wextra quiet until then.
static int eb_write(uint32_t b, const void *buf) __attribute__((unused));
static int eb_write(uint32_t b, const void *buf)
{
    const uint8_t *p = buf;
    if (block_write((uint64_t)b * 2 + 0, p) != 0) { return -1; }
    if (block_write((uint64_t)b * 2 + 1, p + 512) != 0) { return -1; }
    return 0;
}

// --- inode access ---

// Read inode number `n` (1-based) into `out`. Inode N lives in block group
// (N-1)/inodes_per_group, at index (N-1)%inodes_per_group within that group's
// inode table; its byte offset is index*inode_size. We read the one block that
// contains the inode and copy the first 128 bytes (our struct) out of it.
static int read_inode(uint32_t n, struct ext2_inode *out)
{
    if (n == 0) { return -1; }
    uint32_t g = (n - 1) / sb.s_inodes_per_group;
    uint32_t idx = (n - 1) % sb.s_inodes_per_group;
    if (g >= ngroups) { return -1; }

    uint64_t byte = (uint64_t)idx * inode_size;        // offset within the table
    uint32_t blk  = gdt[g].bg_inode_table + (uint32_t)(byte / EXT2_BLOCK_SIZE);
    uint32_t boff = (uint32_t)(byte % EXT2_BLOCK_SIZE);

    uint8_t buf[EXT2_BLOCK_SIZE];
    if (eb_read(blk, buf) != 0) { return -1; }
    // An inode (128 bytes) never straddles a 1024-byte block boundary because
    // inode_size (128 or 256) divides the block size evenly, so this copy is safe.
    uint8_t *dst = (uint8_t *)out;
    for (unsigned i = 0; i < sizeof(struct ext2_inode); i++) { dst[i] = buf[boff + i]; }
    return 0;
}

// --- block map: file block index -> physical block (read-only, no alloc) ---
//
// i_block[0..11] are direct: file block b<12 is just i_block[b]. Beyond that,
// i_block[12] points to a block of 256 block-numbers (single indirect),
// i_block[13] to a block of pointers-to-pointer-blocks (double), and i_block[14]
// to a triple level. A pointer of 0 means a hole (the file is sparse there); we
// return 0 and the read fills zeros.

// Read the entry at `index` of the pointer block at physical block `blk`.
// blk==0 (a hole in an indirect chain) yields 0.
static uint32_t ind_entry(uint32_t blk, uint32_t index)
{
    if (blk == 0) { return 0; }
    uint8_t buf[EXT2_BLOCK_SIZE];
    if (eb_read(blk, buf) != 0) { return 0; }
    uint32_t v;
    const uint8_t *p = buf + index * 4;
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return v;
}

// Map file block index `b` to its physical block (0 == hole). Walks the direct
// pointers, then the single/double/triple indirect trees as the index grows.
static uint32_t bmap(const struct ext2_inode *ino, uint32_t b)
{
    if (b < EXT2_NDIR) { return ino->i_block[b]; }
    b -= EXT2_NDIR;

    // Single indirect: 256 entries.
    if (b < PTRS_PER_BLOCK) {
        return ind_entry(ino->i_block[EXT2_IND], b);
    }
    b -= PTRS_PER_BLOCK;

    // Double indirect: 256 single-indirect blocks => 256*256 entries.
    if (b < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        uint32_t mid = ind_entry(ino->i_block[EXT2_DIND], b / PTRS_PER_BLOCK);
        return ind_entry(mid, b % PTRS_PER_BLOCK);
    }
    b -= PTRS_PER_BLOCK * PTRS_PER_BLOCK;

    // Triple indirect: 256^3 entries. (Far beyond any file we ship, but cheap to
    // support and it makes bmap total over the addressable range.)
    uint32_t hi  = ind_entry(ino->i_block[EXT2_TIND], b / (PTRS_PER_BLOCK * PTRS_PER_BLOCK));
    uint32_t mid = ind_entry(hi, (b / PTRS_PER_BLOCK) % PTRS_PER_BLOCK);
    return ind_entry(mid, b % PTRS_PER_BLOCK);
}

// --- vnodes (vnode.priv holds the 1-based inode number) ---

extern const struct vnode_ops ext2_ops;

static struct vnode *mkvnode(uint32_t inum)
{
    struct ext2_inode ino;
    if (read_inode(inum, &ino) != 0) { return 0; }
    struct vnode *vn = kmalloc(sizeof(struct vnode));
    if (!vn) { return 0; }
    vn->type = ((ino.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? VN_DIR : VN_FILE;
    vn->size = ino.i_size;
    vn->ops  = &ext2_ops;
    vn->priv = (void *)(uintptr_t)inum;
    return vn;
}

static uint32_t inum_of(struct vnode *vn) { return (uint32_t)(uintptr_t)vn->priv; }

// --- read path ---

// Read up to `len` bytes from offset `off`. Clamps to i_size; for each block in
// range, bmap -> eb_read -> copy the slice. A hole (physical block 0) reads as
// zeros, matching ext2 sparse-file semantics.
static int ext2_read(struct vnode *vn, uint64_t off, void *buf, uint64_t len)
{
    struct ext2_inode ino;
    if (read_inode(inum_of(vn), &ino) != 0) { return -1; }
    if (off >= ino.i_size) { return 0; }
    if (off + len > ino.i_size) { len = ino.i_size - off; }

    uint8_t *out = buf;
    uint64_t done = 0;
    uint8_t blk[EXT2_BLOCK_SIZE];
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bi  = (uint32_t)(pos / EXT2_BLOCK_SIZE);  // which file block
        uint32_t boff = (uint32_t)(pos % EXT2_BLOCK_SIZE); // offset within it
        uint64_t n = EXT2_BLOCK_SIZE - boff;               // bytes left in this block
        if (n > len - done) { n = len - done; }

        uint32_t phys = bmap(&ino, bi);
        if (phys == 0) {
            // Hole: bytes read as zeros.
            for (uint64_t i = 0; i < n; i++) { out[done + i] = 0; }
        } else {
            if (eb_read(phys, blk) != 0) { return (int)done; }
            for (uint64_t i = 0; i < n; i++) { out[done + i] = blk[boff + i]; }
        }
        done += n;
    }
    return (int)done;
}

// --- directory ops ---

// Compare a directory entry's (non-NUL-terminated) name of length `len` against
// the NUL-terminated `name`.
static int name_eq(const char *ename, uint8_t len, const char *name)
{
    for (uint8_t i = 0; i < len; i++) {
        if (name[i] == '\0' || name[i] != ename[i]) { return 0; }
    }
    return name[len] == '\0';   // full match only if `name` also ends here
}

// Scan a directory's data blocks for an entry named `name`; return a vnode for
// its inode, or NULL. Entries are a packed linked list within each block, walked
// via rec_len; inode==0 slots are skipped.
static struct vnode *ext2_lookup(struct vnode *dir, const char *name)
{
    struct ext2_inode dino;
    if (read_inode(inum_of(dir), &dino) != 0) { return 0; }

    uint32_t nblocks = (uint32_t)((dino.i_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE);
    uint8_t blk[EXT2_BLOCK_SIZE];
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t phys = bmap(&dino, bi);
        if (phys == 0) { continue; }
        if (eb_read(phys, blk) != 0) { continue; }
        uint32_t o = 0;
        while (o < EXT2_BLOCK_SIZE) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + o);
            if (de->rec_len == 0) { break; }   // malformed: avoid an infinite loop
            if (de->inode != 0 && name_eq(de->name, de->name_len, name)) {
                return mkvnode(de->inode);
            }
            o += de->rec_len;
        }
    }
    return 0;
}

// Return the name of the Nth real directory entry (skipping holes). Includes
// "." and ".."; callers filter. Used by readdir / getdents64.
static int ext2_readdir(struct vnode *dir, int index, char *name_out)
{
    struct ext2_inode dino;
    if (read_inode(inum_of(dir), &dino) != 0) { return -1; }

    uint32_t nblocks = (uint32_t)((dino.i_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE);
    uint8_t blk[EXT2_BLOCK_SIZE];
    int seen = 0;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t phys = bmap(&dino, bi);
        if (phys == 0) { continue; }
        if (eb_read(phys, blk) != 0) { continue; }
        uint32_t o = 0;
        while (o < EXT2_BLOCK_SIZE) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + o);
            if (de->rec_len == 0) { break; }
            if (de->inode != 0) {
                if (seen == index) {
                    uint8_t k = de->name_len;
                    if (k > 31) { k = 31; }
                    for (uint8_t i = 0; i < k; i++) { name_out[i] = de->name[i]; }
                    name_out[k] = '\0';
                    return 0;
                }
                seen++;
            }
            o += de->rec_len;
        }
    }
    return -1;
}

// --- write path: Phase 2. Read-only for now. ---

static int ext2_write(struct vnode *vn, uint64_t off, const void *buf, uint64_t len)
{
    (void)vn; (void)off; (void)buf; (void)len;
    return -1;   // TODO(ext2 Phase 2): allocate + write blocks, grow i_size
}

static struct vnode *ext2_create(struct vnode *dir, const char *name, int type)
{
    (void)dir; (void)name; (void)type;
    return 0;    // TODO(ext2 Phase 2): alloc_inode + insert dir entry
}

static int ext2_truncate(struct vnode *vn)
{
    (void)vn;
    return -1;   // TODO(ext2 Phase 2): free data/indirect blocks, i_size=0
}

const struct vnode_ops ext2_ops = {
    .read = ext2_read, .write = ext2_write,
    .lookup = ext2_lookup, .create = ext2_create, .readdir = ext2_readdir,
    .truncate = ext2_truncate,
};

// --- mount ---

// Read + validate the superblock, cache it and the group-descriptor table, and
// return a vnode for the root directory (inode 2). Returns NULL on a bad magic
// (a blank/zeroed disk or a non-ext2 image) so kmain leaves /disk unmounted.
struct vnode *ext2_mount(void)
{
    // The superblock sits at byte 1024 == ext2 block 1 (first_data_block for
    // 1024-byte blocks). eb_read of block 1 reads sectors 2 and 3.
    if (eb_read(1, &sb) != 0) { return 0; }
    if (sb.s_magic != EXT2_MAGIC) { return 0; }
    // We only handle 1024-byte blocks (the image is built with -b 1024).
    if (sb.s_log_block_size != 0) { return 0; }

    // Inode size: 128 in the old rev (s_inode_size field absent/zero), else the
    // field's value (commonly 256). mke2fs writes a dynamic-rev superblock.
    inode_size = (sb.s_rev_level >= 1 && sb.s_inode_size) ? sb.s_inode_size : 128;

    // Number of block groups = ceil(blocks_count / blocks_per_group), and it must
    // also equal ceil(inodes_count / inodes_per_group); use the block form.
    ngroups = (sb.s_blocks_count - sb.s_first_data_block + sb.s_blocks_per_group - 1)
              / sb.s_blocks_per_group;
    if (ngroups == 0) { return 0; }

    // The group-descriptor table starts in the block right after the superblock
    // (block first_data_block + 1). Each descriptor is 32 bytes; read enough
    // blocks to cover all of them.
    gdt = kmalloc(ngroups * sizeof(struct ext2_group_desc));
    if (!gdt) { return 0; }
    uint32_t gdt_block = sb.s_first_data_block + 1;
    uint32_t per_block = EXT2_BLOCK_SIZE / sizeof(struct ext2_group_desc);  // 32 descs/block
    uint8_t buf[EXT2_BLOCK_SIZE];
    for (uint32_t g = 0; g < ngroups; g++) {
        if (g % per_block == 0) {
            if (eb_read(gdt_block + g / per_block, buf) != 0) { return 0; }
        }
        const uint8_t *src = buf + (g % per_block) * sizeof(struct ext2_group_desc);
        uint8_t *dst = (uint8_t *)&gdt[g];
        for (unsigned i = 0; i < sizeof(struct ext2_group_desc); i++) { dst[i] = src[i]; }
    }

    return mkvnode(EXT2_ROOT_INO);
}

static struct fs_type ext2_fs = { .name = "ext2", .mount = ext2_mount };
struct fs_type *ext2_type(void) { return &ext2_fs; }
