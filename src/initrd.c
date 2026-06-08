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

static const char
hello[] = "Hello, MyOSv2!\n";
static const char motd[]  = "Welcome to MyOSv2.\n";

static const struct initrd_file files[] = {
    { "/hello.txt", hello, sizeof(hello) - 1 },
    { "/motd",      motd,  sizeof(motd)  - 1 },
};

// The embedded user program (flat binary from user/, via build/user_blob.c).
extern unsigned char init_bin[];
extern unsigned int  init_bin_len;

void initrd_unpack(void)
{
    unsigned count = sizeof(files) / sizeof(files[0]);
    for (unsigned i = 0; i < count; i++) {
        struct vnode *vn = vfs_create(files[i].path, VN_FILE);
        if (!vn) { continue; }
        struct file f = { .vnode = vn, .off = 0 };
        vfs_write(&f, files[i].data, files[i].len);
    }

    // Expose the embedded user program as a runnable file at /bin/init.
    vfs_create("/bin", VN_DIR);
    struct vnode *prog = vfs_create("/bin/init", VN_FILE);
    if (prog) {
        struct file pf = { .vnode = prog, .off = 0 };
        vfs_write(&pf, init_bin, (uint64_t)init_bin_len);
    }
}
