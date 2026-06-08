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

// The embedded user programs (ELF executables from user/, via build/user_blob.c).
extern unsigned char sh_elf[];    extern unsigned int sh_elf_len;
extern unsigned char true_elf[];  extern unsigned int true_elf_len;
extern unsigned char false_elf[]; extern unsigned int false_elf_len;
extern unsigned char hello_elf[]; extern unsigned int hello_elf_len;
extern unsigned char mtest_elf[]; extern unsigned int mtest_elf_len;
extern unsigned char shmtest_elf[]; extern unsigned int shmtest_elf_len;

// Write an embedded program into the filesystem at `path`.
static void add_prog(const char *path, const void *data, uint64_t len)
{
    struct vnode *vn = vfs_create(path, VN_FILE);
    if (!vn) { return; }
    struct file f = { .vnode = vn, .off = 0 };
    vfs_write(&f, data, len);
}

void initrd_unpack(void)
{
    unsigned count = sizeof(files) / sizeof(files[0]);
    for (unsigned i = 0; i < count; i++) {
        struct vnode *vn = vfs_create(files[i].path, VN_FILE);
        if (!vn) { continue; }
        struct file f = { .vnode = vn, .off = 0 };
        vfs_write(&f, files[i].data, files[i].len);
    }

    // Expose the embedded programs under /bin. /bin/init is the first process
    // (the shell); /bin/sh is the same program by its conventional name.
    vfs_create("/bin", VN_DIR);
    add_prog("/bin/init",  sh_elf,    (uint64_t)sh_elf_len);
    add_prog("/bin/sh",    sh_elf,    (uint64_t)sh_elf_len);
    add_prog("/bin/true",  true_elf,  (uint64_t)true_elf_len);
    add_prog("/bin/false", false_elf, (uint64_t)false_elf_len);
    add_prog("/bin/hello", hello_elf, (uint64_t)hello_elf_len);
    add_prog("/bin/mtest", mtest_elf, (uint64_t)mtest_elf_len);
    add_prog("/bin/shmtest", shmtest_elf, (uint64_t)shmtest_elf_len);
}
