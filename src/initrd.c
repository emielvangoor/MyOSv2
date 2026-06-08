// initrd.c -- a tiny "initial RAM disk": a table of files compiled into the
// kernel and unpacked into the mounted filesystem at boot. No external build
// tool needed. (Phase 8 adds a program binary here for exec.)
#include <stdint.h>
#include "initrd.h"
#include "vfs.h"

struct initrd_file {
    const char *path;
    const char *data;
    uint64_t len;
};

static const char hello[] = "Hello, files!\n";
static const char motd[]  = "Welcome to MyOSv2.\n";

static const struct initrd_file files[] = {
    { "/hello.txt", hello, sizeof(hello) - 1 },
    { "/motd",      motd,  sizeof(motd)  - 1 },
};

void initrd_unpack(void)
{
    unsigned count = sizeof(files) / sizeof(files[0]);
    for (unsigned i = 0; i < count; i++) {
        struct vnode *vn = vfs_create(files[i].path, VN_FILE);
        if (!vn) { continue; }
        struct file f = { .vnode = vn, .off = 0 };
        vfs_write(&f, files[i].data, files[i].len);
    }
}
