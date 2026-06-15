#!/usr/bin/env python3
"""
mkdisk.py -- build a fresh SFS disk image pre-seeded with /init.l.

A blank disk boots the OS into the serial REPL + the "run lisp -frame" splash.
We want the PERSISTENT disk to boot straight into the graphical frame, so the
Makefile builds disk.img with this tool instead of dd-zero: it lays down a
valid SFS filesystem (mirroring src/sfs.c byte-for-byte) whose root directory
already contains /init.l = (run-bg "lisp" "-frame"). PID 1 loads /init.l on
boot, so the frame comes up by itself -- and since the disk persists, it keeps
doing so (and you can edit /disk/init.l from inside the machine).

SFS layout (512-byte blocks, all little-endian):
  block 0      superblock (struct sfs_super)
  blocks 1..8  inode table, 64 bytes/inode, 8 per block (inode 1 = root dir)
  block 9      root directory data (16 dirents x 32 bytes)
  block 10..   file data

    python3 tools/mkdisk.py disk.img
"""

import struct
import sys

BLK = 512
NBLOCKS = 8192                  # 4 MiB / 512, matches sfs_mkfs's super.nblocks
INODE_START, INODE_BLOCKS, DATA_START = 1, 8, 9
MAGIC = 0x53465331              # "SFS1"

# What the machine runs on boot. Edit /disk/init.l from inside the OS to change
# it; this is just the factory default a brand-new disk ships with.
INIT_L = b'(run-bg "lisp" "-frame")\n'


def inode(typ, size, direct0):
    """One 64-byte inode: type, size, direct[12], pad[2]."""
    direct = [0] * 12
    direct[0] = direct0
    return struct.pack("<2I12I2I", typ, size, *direct, 0, 0)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "disk.img"
    img = bytearray(NBLOCKS * BLK)

    # Block 0: superblock. next_inode/next_block point PAST what we seed (root
    # inode 1, init.l inode 2; root-dir block 9, init.l block 10) so the kernel's
    # own allocations (e.g. the /disk/boots counter) never clobber our files.
    img[0:32] = struct.pack("<8I", MAGIC, INODE_BLOCKS * 8, INODE_START,
                            INODE_BLOCKS, DATA_START, NBLOCKS, 3, 11)

    # Block 1 holds inodes 0..7; write inode 1 (root dir) and inode 2 (init.l).
    b1 = INODE_START * BLK
    img[b1 + 64:b1 + 128]  = inode(2, 0, DATA_START)        # inode 1: root dir
    img[b1 + 128:b1 + 192] = inode(1, len(INIT_L), 10)      # inode 2: init.l file

    # Block 9: the root directory -- a single entry, "init.l" -> inode 2.
    d = DATA_START * BLK
    img[d:d + 4] = struct.pack("<I", 2)
    img[d + 4:d + 4 + len(b"init.l")] = b"init.l"

    # Block 10: init.l's contents.
    img[10 * BLK:10 * BLK + len(INIT_L)] = INIT_L

    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out}: SFS pre-seeded with /init.l = {INIT_L!r}")


if __name__ == "__main__":
    main()
