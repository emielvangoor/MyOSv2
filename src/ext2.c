// ext2.c -- the ext2 read driver, behind the VFS vnode_ops.
// =========================================================
//
// This is the sole consumer of block_read/block_write -- the root filesystem `/`
// lives on the block device. It speaks the ext2 on-disk format directly: a
// superblock describing the geometry, a table of block-group descriptors,
// per-group inode tables, and inodes whose i_block[] array maps file offsets to
// physical blocks (direct + indirect).
//
// WHY ext2 and not something homegrown: ext2 is exhaustively documented and --
// decisively -- the host's e2fsprogs can BUILD and pre-populate an image
// (`mke2fs -d build/rootfs`), so the root / arrives already containing the
// userland (/bin, /lib, /usr), /init.l, and the KTEST fixtures -- the kernel
// just mounts it and reads what the host laid down.
//
// Block size is 1024 bytes (forced with `mke2fs -b 1024`); the block device
// speaks 512-byte sectors, so one ext2 block = 2 sectors. eb_read/eb_write do
// that 2-sector transfer.
//
// Phase 2 adds the WRITE path: bitmap allocation (alloc_block/alloc_inode and
// their frees), bmap_alloc (block map that allocates data + indirect blocks),
// and write/create/truncate/unlink. All metadata is write-through -- after any
// mutation we persist the bitmap, group descriptor, superblock, and inode, so a
// remount (or a host e2fsck) sees a consistent filesystem.

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

// Full i_mode values we write when creating: type bits | default permissions.
#define EXT2_MODE_REG   0x81A4      // regular file, rw-r--r-- (0644)
#define EXT2_MODE_DIR   0x41ED      // directory,    rwxr-xr-x (0755)

// Directory-entry file_type codes (the "_2" dir-entry variant). Read ignores
// these, but mke2fs/Linux expect them set, so we write them on create.
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2

// Sectors per ext2 block, in 512-byte units (i_blocks counts these).
#define SECTORS_PER_BLOCK (EXT2_BLOCK_SIZE / 512)   // 2

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

// Write ext2 block `b` as two sector writes.
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

// Write inode number `n` (1-based) back to disk. Mirrors read_inode's geometry:
// locate the block holding the inode, read-modify-write the 128-byte struct in
// place (preserving any trailing bytes of a 256-byte on-disk inode we don't model).
static int write_inode(uint32_t n, const struct ext2_inode *in)
{
    if (n == 0) { return -1; }
    uint32_t g = (n - 1) / sb.s_inodes_per_group;
    uint32_t idx = (n - 1) % sb.s_inodes_per_group;
    if (g >= ngroups) { return -1; }

    uint64_t byte = (uint64_t)idx * inode_size;
    uint32_t blk  = gdt[g].bg_inode_table + (uint32_t)(byte / EXT2_BLOCK_SIZE);
    uint32_t boff = (uint32_t)(byte % EXT2_BLOCK_SIZE);

    uint8_t buf[EXT2_BLOCK_SIZE];
    if (eb_read(blk, buf) != 0) { return -1; }
    const uint8_t *src = (const uint8_t *)in;
    for (unsigned i = 0; i < sizeof(struct ext2_inode); i++) { buf[boff + i] = src[i]; }
    return eb_write(blk, buf);
}

// --- superblock + group-descriptor write-back ---
//
// Both are cached in RAM (sb, gdt[]); after we mutate a free-count we persist
// the change so a remount (or a host e2fsck) sees a consistent filesystem.

// Persist the in-RAM superblock to block 1 (byte offset 1024).
static int write_super(void)
{
    return eb_write(sb.s_first_data_block, &sb);
}

// Persist the descriptor for group `g` back into the on-disk descriptor table.
// The table starts at first_data_block+1; 32 descriptors fit per 1024-byte block.
static int write_group_desc(uint32_t g)
{
    uint32_t per_block = EXT2_BLOCK_SIZE / sizeof(struct ext2_group_desc);
    uint32_t gdt_block = sb.s_first_data_block + 1 + g / per_block;
    uint8_t buf[EXT2_BLOCK_SIZE];
    if (eb_read(gdt_block, buf) != 0) { return -1; }
    uint8_t *dst = buf + (g % per_block) * sizeof(struct ext2_group_desc);
    const uint8_t *src = (const uint8_t *)&gdt[g];
    for (unsigned i = 0; i < sizeof(struct ext2_group_desc); i++) { dst[i] = src[i]; }
    return eb_write(gdt_block, buf);
}

// --- bitmap allocation (Phase 2) ---
//
// Each group has a block bitmap and an inode bitmap, one bit per block/inode in
// the group; bit set == in use. We scan for the first clear bit, set it, and
// decrement the group + superblock free counts (write-through). Bit i in a
// bitmap byte is at byte i/8, mask 1<<(i%8) -- the ext2 convention.

// Find the first clear bit in `nbits` bits of `bm`; return its index or -1.
static int bitmap_find_free(const uint8_t *bm, uint32_t nbits)
{
    for (uint32_t i = 0; i < nbits; i++) {
        if (!(bm[i / 8] & (1u << (i % 8)))) { return (int)i; }
    }
    return -1;
}

// Allocate a fresh data block. Scan each group's block bitmap for a free bit,
// claim it, persist the bitmap + counts, and return the physical block number
// (or 0 on out-of-space). New blocks are handed out low-first, so the high
// scratch sectors the `block:` tests scribble on are never returned here.
static uint32_t alloc_block(void)
{
    uint8_t bm[EXT2_BLOCK_SIZE];
    for (uint32_t g = 0; g < ngroups; g++) {
        if (gdt[g].bg_free_blocks_count == 0) { continue; }
        if (eb_read(gdt[g].bg_block_bitmap, bm) != 0) { return 0; }
        // Only the first blocks_per_group bits are meaningful (the last group may
        // be short, but its bitmap pads the tail bits as in-use, so scanning the
        // full group width is safe).
        uint32_t nbits = sb.s_blocks_per_group;
        int bit = bitmap_find_free(bm, nbits);
        if (bit < 0) { continue; }
        bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
        if (eb_write(gdt[g].bg_block_bitmap, bm) != 0) { return 0; }
        gdt[g].bg_free_blocks_count--;
        sb.s_free_blocks_count--;
        write_group_desc(g);
        write_super();
        // Bit `bit` of group g maps to physical block:
        //   g*blocks_per_group + first_data_block + bit
        return g * sb.s_blocks_per_group + sb.s_first_data_block + (uint32_t)bit;
    }
    return 0;   // disk full
}

// Free data block `b`: clear its bitmap bit, bump the free counts. Inverse of
// alloc_block's bit math.
static void free_block(uint32_t b)
{
    if (b < sb.s_first_data_block) { return; }
    uint32_t rel = b - sb.s_first_data_block;
    uint32_t g   = rel / sb.s_blocks_per_group;
    uint32_t bit = rel % sb.s_blocks_per_group;
    if (g >= ngroups) { return; }
    uint8_t bm[EXT2_BLOCK_SIZE];
    if (eb_read(gdt[g].bg_block_bitmap, bm) != 0) { return; }
    if (!(bm[bit / 8] & (1u << (bit % 8)))) { return; }   // already free
    bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    if (eb_write(gdt[g].bg_block_bitmap, bm) != 0) { return; }
    gdt[g].bg_free_blocks_count++;
    sb.s_free_blocks_count++;
    write_group_desc(g);
    write_super();
}

// Allocate a fresh inode. Like alloc_block but over the inode bitmap, and it
// must never hand out a reserved inode (< s_first_ino, e.g. the root). The
// inode bitmap has inodes_per_group bits; bit i of group g is inode number
// g*inodes_per_group + i + 1 (inodes are 1-based). `is_dir` bumps the group's
// used-dirs count, which e2fsck checks. Returns the inode number or 0.
static uint32_t alloc_inode(int is_dir)
{
    uint8_t bm[EXT2_BLOCK_SIZE];
    for (uint32_t g = 0; g < ngroups; g++) {
        if (gdt[g].bg_free_inodes_count == 0) { continue; }
        if (eb_read(gdt[g].bg_inode_bitmap, bm) != 0) { return 0; }
        uint32_t nbits = sb.s_inodes_per_group;
        for (uint32_t i = 0; i < nbits; i++) {
            if (bm[i / 8] & (1u << (i % 8))) { continue; }     // in use
            uint32_t inum = g * sb.s_inodes_per_group + i + 1; // 1-based
            if (inum < sb.s_first_ino) { continue; }           // never reserved inodes
            bm[i / 8] |= (uint8_t)(1u << (i % 8));
            if (eb_write(gdt[g].bg_inode_bitmap, bm) != 0) { return 0; }
            gdt[g].bg_free_inodes_count--;
            if (is_dir) { gdt[g].bg_used_dirs_count++; }
            sb.s_free_inodes_count--;
            write_group_desc(g);
            write_super();
            return inum;
        }
    }
    return 0;
}

// Free inode `n`: clear its bitmap bit, bump the free count, (and the used-dirs
// count if it was a directory).
static void free_inode(uint32_t n, int was_dir)
{
    if (n == 0) { return; }
    uint32_t g   = (n - 1) / sb.s_inodes_per_group;
    uint32_t bit = (n - 1) % sb.s_inodes_per_group;
    if (g >= ngroups) { return; }
    uint8_t bm[EXT2_BLOCK_SIZE];
    if (eb_read(gdt[g].bg_inode_bitmap, bm) != 0) { return; }
    bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    if (eb_write(gdt[g].bg_inode_bitmap, bm) != 0) { return; }
    gdt[g].bg_free_inodes_count++;
    if (was_dir && gdt[g].bg_used_dirs_count > 0) { gdt[g].bg_used_dirs_count--; }
    sb.s_free_inodes_count++;
    write_group_desc(g);
    write_super();
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

// --- block map with allocation (Phase 2) ---
//
// bmap_alloc maps file block `b` to a physical block, allocating the data block
// AND any missing indirect blocks along the way. It mutates *ino (i_block[],
// i_blocks) in memory; the caller persists the inode with write_inode after.
// Returns the physical block, or 0 on out-of-space.
//
// Helpers below resolve one "level" of the indirect tree: given a slot that
// holds a child pointer (either an i_block[] entry or an entry inside a pointer
// block), ensure that child exists, allocating + zeroing a fresh block if the
// slot is 0. `*alloced` is set when a new block was claimed (so i_blocks grows).

// Read one u32 from pointer-block `blk` at `index`.
static uint32_t ptr_get(uint32_t blk, uint32_t index)
{
    uint8_t buf[EXT2_BLOCK_SIZE];
    if (eb_read(blk, buf) != 0) { return 0; }
    const uint8_t *p = buf + index * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Write one u32 into pointer-block `blk` at `index` (read-modify-write).
static int ptr_set(uint32_t blk, uint32_t index, uint32_t val)
{
    uint8_t buf[EXT2_BLOCK_SIZE];
    if (eb_read(blk, buf) != 0) { return -1; }
    uint8_t *p = buf + index * 4;
    p[0] = (uint8_t)val; p[1] = (uint8_t)(val >> 8);
    p[2] = (uint8_t)(val >> 16); p[3] = (uint8_t)(val >> 24);
    return eb_write(blk, buf);
}

// Allocate and zero a fresh block (used for data and indirect blocks alike).
// Zeroing matters for indirect blocks: their pointer slots must read as 0
// (holes) until filled, and for data blocks it avoids leaking stale disk bytes.
static uint32_t alloc_zeroed_block(void)
{
    uint32_t b = alloc_block();
    if (b == 0) { return 0; }
    uint8_t zero[EXT2_BLOCK_SIZE];
    for (int i = 0; i < EXT2_BLOCK_SIZE; i++) { zero[i] = 0; }
    if (eb_write(b, zero) != 0) { return 0; }
    return b;
}

// Map+allocate file block `b`. Walks direct then single/double/triple indirect,
// allocating each missing link. Adds SECTORS_PER_BLOCK to i_blocks per newly
// claimed block (data or indirect). Returns the physical data block, or 0.
static uint32_t bmap_alloc(struct ext2_inode *ino, uint32_t b)
{
    // Direct.
    if (b < EXT2_NDIR) {
        if (ino->i_block[b] == 0) {
            uint32_t nb = alloc_zeroed_block();
            if (nb == 0) { return 0; }
            ino->i_block[b] = nb;
            ino->i_blocks += SECTORS_PER_BLOCK;
        }
        return ino->i_block[b];
    }
    b -= EXT2_NDIR;

    // Single indirect: i_block[12] -> pointer block of 256 data blocks.
    if (b < PTRS_PER_BLOCK) {
        if (ino->i_block[EXT2_IND] == 0) {
            uint32_t nb = alloc_zeroed_block();
            if (nb == 0) { return 0; }
            ino->i_block[EXT2_IND] = nb;
            ino->i_blocks += SECTORS_PER_BLOCK;
        }
        uint32_t ind = ino->i_block[EXT2_IND];
        uint32_t data = ptr_get(ind, b);
        if (data == 0) {
            data = alloc_zeroed_block();
            if (data == 0) { return 0; }
            if (ptr_set(ind, b, data) != 0) { return 0; }
            ino->i_blocks += SECTORS_PER_BLOCK;
        }
        return data;
    }
    b -= PTRS_PER_BLOCK;

    // Double indirect: i_block[13] -> block of 256 single-indirect blocks.
    if (b < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (ino->i_block[EXT2_DIND] == 0) {
            uint32_t nb = alloc_zeroed_block();
            if (nb == 0) { return 0; }
            ino->i_block[EXT2_DIND] = nb;
            ino->i_blocks += SECTORS_PER_BLOCK;
        }
        uint32_t dind = ino->i_block[EXT2_DIND];
        uint32_t mid = ptr_get(dind, b / PTRS_PER_BLOCK);
        if (mid == 0) {
            mid = alloc_zeroed_block();
            if (mid == 0) { return 0; }
            if (ptr_set(dind, b / PTRS_PER_BLOCK, mid) != 0) { return 0; }
            ino->i_blocks += SECTORS_PER_BLOCK;
        }
        uint32_t data = ptr_get(mid, b % PTRS_PER_BLOCK);
        if (data == 0) {
            data = alloc_zeroed_block();
            if (data == 0) { return 0; }
            if (ptr_set(mid, b % PTRS_PER_BLOCK, data) != 0) { return 0; }
            ino->i_blocks += SECTORS_PER_BLOCK;
        }
        return data;
    }
    b -= PTRS_PER_BLOCK * PTRS_PER_BLOCK;

    // Triple indirect: i_block[14] -> block of double-indirect blocks. (Far past
    // anything we ship, but kept total so bmap_alloc never silently fails.)
    if (ino->i_block[EXT2_TIND] == 0) {
        uint32_t nb = alloc_zeroed_block();
        if (nb == 0) { return 0; }
        ino->i_block[EXT2_TIND] = nb;
        ino->i_blocks += SECTORS_PER_BLOCK;
    }
    uint32_t tind = ino->i_block[EXT2_TIND];
    uint32_t hi_i = b / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
    uint32_t hi = ptr_get(tind, hi_i);
    if (hi == 0) {
        hi = alloc_zeroed_block();
        if (hi == 0) { return 0; }
        if (ptr_set(tind, hi_i, hi) != 0) { return 0; }
        ino->i_blocks += SECTORS_PER_BLOCK;
    }
    uint32_t mid_i = (b / PTRS_PER_BLOCK) % PTRS_PER_BLOCK;
    uint32_t mid = ptr_get(hi, mid_i);
    if (mid == 0) {
        mid = alloc_zeroed_block();
        if (mid == 0) { return 0; }
        if (ptr_set(hi, mid_i, mid) != 0) { return 0; }
        ino->i_blocks += SECTORS_PER_BLOCK;
    }
    uint32_t data = ptr_get(mid, b % PTRS_PER_BLOCK);
    if (data == 0) {
        data = alloc_zeroed_block();
        if (data == 0) { return 0; }
        if (ptr_set(mid, b % PTRS_PER_BLOCK, data) != 0) { return 0; }
        ino->i_blocks += SECTORS_PER_BLOCK;
    }
    return data;
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

// --- write path (Phase 2) ---

// Write `len` bytes from `buf` at offset `off`, allocating blocks as needed and
// growing i_size. For each touched file block: bmap_alloc -> read-modify-write
// the slice. The inode (i_block[]/i_blocks updated by bmap_alloc, plus i_size)
// is persisted once at the end. Returns bytes written, or -1 on a fatal error.
static int ext2_write(struct vnode *vn, uint64_t off, const void *buf, uint64_t len)
{
    struct ext2_inode ino;
    uint32_t inum = inum_of(vn);
    if (read_inode(inum, &ino) != 0) { return -1; }

    const uint8_t *src = buf;
    uint64_t done = 0;
    uint8_t blk[EXT2_BLOCK_SIZE];
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bi   = (uint32_t)(pos / EXT2_BLOCK_SIZE);   // which file block
        uint32_t boff = (uint32_t)(pos % EXT2_BLOCK_SIZE);   // offset within it
        uint64_t n = EXT2_BLOCK_SIZE - boff;                 // bytes left in block
        if (n > len - done) { n = len - done; }

        uint32_t phys = bmap_alloc(&ino, bi);
        if (phys == 0) { break; }   // out of space: stop, report what we wrote

        // Read-modify-write: a partial-block write must preserve the bytes around
        // the slice. A full-block write (boff==0 && n==BLOCK) could skip the read,
        // but the freshly-allocated block is already zeroed, so this is correct
        // either way and simpler.
        if (eb_read(phys, blk) != 0) { break; }
        for (uint64_t i = 0; i < n; i++) { blk[boff + i] = src[done + i]; }
        if (eb_write(phys, blk) != 0) { break; }
        done += n;
    }

    if (off + done > ino.i_size) { ino.i_size = (uint32_t)(off + done); }
    if (write_inode(inum, &ino) != 0) { return -1; }
    vn->size = ino.i_size;        // keep the cached vnode size in step
    return (int)done;
}

// Free every block an inode references -- direct, then the single/double/triple
// indirect trees (freeing the data blocks AND the pointer blocks themselves) --
// and zero i_block[]. Shared by truncate and unlink. Does NOT write the inode
// back (the caller does, after also resetting i_size/i_blocks).
static void free_inode_blocks(struct ext2_inode *ino)
{
    // Direct blocks.
    for (int i = 0; i < EXT2_NDIR; i++) {
        if (ino->i_block[i]) { free_block(ino->i_block[i]); ino->i_block[i] = 0; }
    }
    // Single indirect: free each data block, then the pointer block.
    if (ino->i_block[EXT2_IND]) {
        uint32_t ind = ino->i_block[EXT2_IND];
        for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
            uint32_t d = ptr_get(ind, i);
            if (d) { free_block(d); }
        }
        free_block(ind);
        ino->i_block[EXT2_IND] = 0;
    }
    // Double indirect: two pointer levels above the data.
    if (ino->i_block[EXT2_DIND]) {
        uint32_t dind = ino->i_block[EXT2_DIND];
        for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
            uint32_t mid = ptr_get(dind, i);
            if (!mid) { continue; }
            for (uint32_t j = 0; j < PTRS_PER_BLOCK; j++) {
                uint32_t d = ptr_get(mid, j);
                if (d) { free_block(d); }
            }
            free_block(mid);
        }
        free_block(dind);
        ino->i_block[EXT2_DIND] = 0;
    }
    // Triple indirect: three pointer levels.
    if (ino->i_block[EXT2_TIND]) {
        uint32_t tind = ino->i_block[EXT2_TIND];
        for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
            uint32_t hi = ptr_get(tind, i);
            if (!hi) { continue; }
            for (uint32_t j = 0; j < PTRS_PER_BLOCK; j++) {
                uint32_t mid = ptr_get(hi, j);
                if (!mid) { continue; }
                for (uint32_t k = 0; k < PTRS_PER_BLOCK; k++) {
                    uint32_t d = ptr_get(mid, k);
                    if (d) { free_block(d); }
                }
                free_block(mid);
            }
            free_block(hi);
        }
        free_block(tind);
        ino->i_block[EXT2_TIND] = 0;
    }
}

// O_TRUNC: free all data + indirect blocks and reset the file to empty. Re-saving
// then writes fresh contents from offset 0, so a shorter rewrite leaves no stale
// tail -- matching the ramfs/sfs truncate semantics.
static int ext2_truncate(struct vnode *vn)
{
    struct ext2_inode ino;
    uint32_t inum = inum_of(vn);
    if (read_inode(inum, &ino) != 0) { return -1; }
    free_inode_blocks(&ino);
    ino.i_size = 0;
    ino.i_blocks = 0;
    if (write_inode(inum, &ino) != 0) { return -1; }
    vn->size = 0;
    return 0;
}

// Round up to the 4-byte alignment ext2 directory entries use: an entry's real
// length is 8 (header) + name_len, rounded up to a multiple of 4.
static uint32_t de_real_len(uint8_t name_len)
{
    return ((uint32_t)8 + name_len + 3) & ~3u;
}

// Append a directory entry (inode `child`, `name`, `ftype`) into directory inode
// `dino` (number `dinum`). ext2 directories are blocks of packed entries whose
// rec_len chains to the next; the last entry's rec_len spans to the block end,
// holding the block's free "slack". To insert: find an entry whose slack
// (rec_len - de_real_len(its name)) fits the new entry, shrink it to its real
// length, and place the new entry in the freed tail. If no block has room,
// allocate a fresh dir block holding one entry spanning the whole block. Updates
// *dino (i_size grows when a block is added) but does not write it back.
static int dir_add_entry(struct ext2_inode *dino, uint32_t dinum,
                         uint32_t child, const char *name, uint8_t name_len,
                         uint8_t ftype)
{
    (void)dinum;
    uint32_t need = de_real_len(name_len);
    uint32_t nblocks = (uint32_t)((dino->i_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE);
    uint8_t blk[EXT2_BLOCK_SIZE];

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t phys = bmap(dino, bi);
        if (phys == 0) { continue; }
        if (eb_read(phys, blk) != 0) { continue; }
        uint32_t o = 0;
        while (o < EXT2_BLOCK_SIZE) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + o);
            if (de->rec_len == 0) { break; }
            uint32_t used = (de->inode == 0) ? 0 : de_real_len(de->name_len);
            uint32_t slack = de->rec_len - used;
            if (slack >= need) {
                struct ext2_dir_entry *nd;
                if (de->inode == 0) {
                    // An empty slot: reuse it whole.
                    nd = de;
                    nd->rec_len = de->rec_len;        // keep its span
                } else {
                    // Split: shrink the occupant to its real length; the new
                    // entry takes the freed tail (the rest of the old rec_len).
                    uint16_t old_rec = de->rec_len;
                    de->rec_len = (uint16_t)used;
                    nd = (struct ext2_dir_entry *)(blk + o + used);
                    nd->rec_len = (uint16_t)(old_rec - used);
                }
                nd->inode = child;
                nd->name_len = name_len;
                nd->file_type = ftype;
                for (uint8_t i = 0; i < name_len; i++) { nd->name[i] = name[i]; }
                return eb_write(phys, blk);
            }
            o += de->rec_len;
        }
    }

    // No room in any existing block: allocate a new dir block with one entry
    // spanning the whole block, and extend the directory's size by one block.
    uint32_t bi = nblocks;
    uint32_t phys = bmap_alloc(dino, bi);
    if (phys == 0) { return -1; }
    for (int i = 0; i < EXT2_BLOCK_SIZE; i++) { blk[i] = 0; }
    struct ext2_dir_entry *nd = (struct ext2_dir_entry *)blk;
    nd->inode = child;
    nd->rec_len = EXT2_BLOCK_SIZE;
    nd->name_len = name_len;
    nd->file_type = ftype;
    for (uint8_t i = 0; i < name_len; i++) { nd->name[i] = name[i]; }
    if (eb_write(phys, blk) != 0) { return -1; }
    dino->i_size += EXT2_BLOCK_SIZE;
    return 0;
}

// Create a file (type VN_FILE) or directory (VN_DIR) named `name` in `dir`.
// Allocates an inode, initializes it, (for a dir) lays down `.`/`..` and adjusts
// link counts, then inserts a directory entry into `dir`. Returns a vnode for
// the new inode, or NULL on failure.
static struct vnode *ext2_create(struct vnode *dir, const char *name, int type)
{
    uint32_t dinum = inum_of(dir);
    struct ext2_inode dino;
    if (read_inode(dinum, &dino) != 0) { return 0; }

    int is_dir = (type == VN_DIR);
    uint32_t inum = alloc_inode(is_dir);
    if (inum == 0) { return 0; }

    // Initialize the new inode from scratch (all zero, then the fields we set).
    struct ext2_inode ino;
    for (unsigned i = 0; i < sizeof(ino); i++) { ((uint8_t *)&ino)[i] = 0; }
    ino.i_mode = is_dir ? EXT2_MODE_DIR : EXT2_MODE_REG;
    ino.i_links_count = is_dir ? 2 : 1;   // a dir's own "." is the 2nd link
    ino.i_size = 0;
    ino.i_blocks = 0;

    uint8_t name_len = 0;
    while (name[name_len] && name_len < 255) { name_len++; }

    if (is_dir) {
        // A directory needs an initial data block with "." (self) and ".."
        // (parent). Both span the rest of the block via rec_len.
        uint32_t db = bmap_alloc(&ino, 0);
        if (db == 0) { free_inode(inum, 1); return 0; }
        ino.i_size = EXT2_BLOCK_SIZE;
        uint8_t blk[EXT2_BLOCK_SIZE];
        for (int i = 0; i < EXT2_BLOCK_SIZE; i++) { blk[i] = 0; }
        struct ext2_dir_entry *dot = (struct ext2_dir_entry *)blk;
        dot->inode = inum; dot->rec_len = 12; dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR; dot->name[0] = '.';
        struct ext2_dir_entry *dd = (struct ext2_dir_entry *)(blk + 12);
        dd->inode = dinum; dd->rec_len = EXT2_BLOCK_SIZE - 12; dd->name_len = 2;
        dd->file_type = EXT2_FT_DIR; dd->name[0] = '.'; dd->name[1] = '.';
        if (eb_write(db, blk) != 0) { free_inode_blocks(&ino); free_inode(inum, 1); return 0; }
    }
    if (write_inode(inum, &ino) != 0) {
        if (is_dir) { free_inode_blocks(&ino); }
        free_inode(inum, is_dir);
        return 0;
    }

    // Link the new inode into the parent directory.
    if (dir_add_entry(&dino, dinum, inum, name, name_len,
                      is_dir ? EXT2_FT_DIR : EXT2_FT_REG) != 0) {
        // Roll back the inode allocation on failure.
        if (is_dir) { free_inode_blocks(&ino); }
        free_inode(inum, is_dir);
        return 0;
    }
    if (is_dir) {
        // ".." in the child adds a link to the parent; bump the parent's count.
        dino.i_links_count++;
    }
    if (write_inode(dinum, &dino) != 0) { return 0; }
    dir->size = dino.i_size;

    return mkvnode(inum);
}

// Remove directory entry `name` from `dir`. Merges the entry's rec_len into the
// previous entry (so the slot is reclaimed); if it is the first entry in its
// block, just zero its inode (a hole). Then decrement the target's link count
// and, at zero links, free its blocks + the inode. Returns 0 / -1 (not found).
int ext2_unlink(struct vnode *dir, const char *name)
{
    uint32_t dinum = inum_of(dir);
    struct ext2_inode dino;
    if (read_inode(dinum, &dino) != 0) { return -1; }

    uint32_t nblocks = (uint32_t)((dino.i_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE);
    uint8_t blk[EXT2_BLOCK_SIZE];
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t phys = bmap(&dino, bi);
        if (phys == 0) { continue; }
        if (eb_read(phys, blk) != 0) { continue; }
        uint32_t o = 0, prev = 0;
        int have_prev = 0;
        while (o < EXT2_BLOCK_SIZE) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + o);
            if (de->rec_len == 0) { break; }
            if (de->inode != 0 && name_eq(de->name, de->name_len, name)) {
                uint32_t target = de->inode;
                // rmdir isn't implemented: refuse to unlink a directory.
                // Removing one here would LEAK it -- a dir's link count starts
                // at >=2 (its own "." + the parent's ".."), and we don't drop
                // the parent's link, so it would never reach 0. (EISDIR.)
                struct ext2_inode tchk;
                if (read_inode(target, &tchk) == 0 &&
                    (tchk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) { return -1; }
                if (have_prev) {
                    // Absorb this entry's span into the previous one.
                    struct ext2_dir_entry *pd = (struct ext2_dir_entry *)(blk + prev);
                    pd->rec_len = (uint16_t)(pd->rec_len + de->rec_len);
                } else {
                    de->inode = 0;   // first entry in block: leave a hole
                }
                if (eb_write(phys, blk) != 0) { return -1; }

                // Decrement the target's link count; free it at zero.
                struct ext2_inode tino;
                if (read_inode(target, &tino) == 0) {
                    if (tino.i_links_count > 0) { tino.i_links_count--; }
                    int was_dir = (tino.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
                    if (tino.i_links_count == 0) {
                        free_inode_blocks(&tino);
                        tino.i_size = 0; tino.i_blocks = 0;
                        write_inode(target, &tino);
                        free_inode(target, was_dir);
                    } else {
                        write_inode(target, &tino);
                    }
                }
                return 0;
            }
            prev = o; have_prev = 1;
            o += de->rec_len;
        }
    }
    return -1;
}

const struct vnode_ops ext2_ops = {
    .read = ext2_read, .write = ext2_write,
    .lookup = ext2_lookup, .create = ext2_create, .readdir = ext2_readdir,
    .truncate = ext2_truncate, .unlink = ext2_unlink,
};

// --- mount ---

// Read + validate the superblock, cache it and the group-descriptor table, and
// return a vnode for the root directory (inode 2). Returns NULL on a bad magic
// (a blank/zeroed disk or a non-ext2 image); since this is now the ROOT
// filesystem, kmain treats that NULL as fatal and halts (no root => no boot).
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
