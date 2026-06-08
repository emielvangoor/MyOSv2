// block.h -- a tiny block-device interface (512-byte sectors) over virtio-blk.
#pragma once
#include <stdint.h>

#define BLOCK_SIZE 512

void virtio_blk_init(void);                          // probe + set up the disk
int  block_present(void);                            // is a disk attached?
int  block_read(uint64_t sector, void *buf);         // read 512 bytes; 0 ok, -1 err
int  block_write(uint64_t sector, const void *buf);  // write 512 bytes; 0 ok, -1 err
