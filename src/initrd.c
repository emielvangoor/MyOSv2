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
extern unsigned char wc_elf[];      extern unsigned int wc_elf_len;
extern unsigned char loop_elf[];    extern unsigned int loop_elf_len;
extern unsigned char catch_elf[];   extern unsigned int catch_elf_len;
extern unsigned char ping_elf[];    extern unsigned int ping_elf_len;
extern unsigned char dnsq_elf[];    extern unsigned int dnsq_elf_len;
extern unsigned char http_elf[];    extern unsigned int http_elf_len;
extern unsigned char httpd_elf[];   extern unsigned int httpd_elf_len;
extern unsigned char polldemo_elf[]; extern unsigned int polldemo_elf_len;
extern unsigned char lm_elf[];      extern unsigned int lm_elf_len;
extern unsigned char evtest_elf[];  extern unsigned int evtest_elf_len;
extern unsigned char gfxtest_elf[]; extern unsigned int gfxtest_elf_len;
extern unsigned char surftest_elf[]; extern unsigned int surftest_elf_len;
extern unsigned char fptest_elf[];  extern unsigned int fptest_elf_len;

// The embedded Lisp source (from user/lisp/*.l, via build/lisp_blob.c).
extern unsigned char bootstrap_l[]; extern unsigned int bootstrap_l_len;
extern unsigned char system_l[];    extern unsigned int system_l_len;
extern unsigned char frame_l[];     extern unsigned int frame_l_len;

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

    // Expose the embedded programs under /bin. /bin/init is the first process:
    // since Phase 24.4 that is the LISP MACHINE -- the OS boots into a Lisp
    // REPL, and the C shell survives as an ordinary command at /bin/sh
    // ((run "sh") from Lisp). The Symbolics inversion, on a Unix-shaped kernel.
    vfs_create("/bin", VN_DIR);
    add_prog("/bin/init",  lm_elf,    (uint64_t)lm_elf_len);
    add_prog("/bin/sh",    sh_elf,    (uint64_t)sh_elf_len);
    add_prog("/bin/true",  true_elf,  (uint64_t)true_elf_len);
    add_prog("/bin/false", false_elf, (uint64_t)false_elf_len);
    add_prog("/bin/hello", hello_elf, (uint64_t)hello_elf_len);
    add_prog("/bin/mtest", mtest_elf, (uint64_t)mtest_elf_len);
    add_prog("/bin/shmtest", shmtest_elf, (uint64_t)shmtest_elf_len);
    add_prog("/bin/wc", wc_elf, (uint64_t)wc_elf_len);
    add_prog("/bin/loop", loop_elf, (uint64_t)loop_elf_len);
    add_prog("/bin/catch", catch_elf, (uint64_t)catch_elf_len);
    add_prog("/bin/ping", ping_elf, (uint64_t)ping_elf_len);
    add_prog("/bin/dnsq", dnsq_elf, (uint64_t)dnsq_elf_len);
    add_prog("/bin/http", http_elf, (uint64_t)http_elf_len);
    add_prog("/bin/httpd", httpd_elf, (uint64_t)httpd_elf_len);
    add_prog("/bin/polldemo", polldemo_elf, (uint64_t)polldemo_elf_len);
    add_prog("/bin/lisp", lm_elf, (uint64_t)lm_elf_len);
    add_prog("/bin/evtest", evtest_elf, (uint64_t)evtest_elf_len);
    add_prog("/bin/gfxtest", gfxtest_elf, (uint64_t)gfxtest_elf_len);
    add_prog("/bin/surftest", surftest_elf, (uint64_t)surftest_elf_len);
    add_prog("/bin/fptest", fptest_elf, (uint64_t)fptest_elf_len);

    // The Lisp standard library (bootstrap.l = the language, system.l = the
    // shell), loaded by /bin/lisp at startup.
    vfs_create("/lib", VN_DIR);
    add_prog("/lib/bootstrap.l", bootstrap_l, (uint64_t)bootstrap_l_len);
    add_prog("/lib/system.l",    system_l,    (uint64_t)system_l_len);
    add_prog("/lib/frame.l",     frame_l,     (uint64_t)frame_l_len);
}
