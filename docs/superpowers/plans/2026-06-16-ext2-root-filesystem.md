# ext2 as Root Filesystem — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `/` a persistent ext2 filesystem (built/installed at image-build time, mounted by the kernel) so on-device edits survive reboots, removing the ramfs root and the kernel-embedded userland.

**Architecture:** The disk image *is* the installed root filesystem. The Makefile stages the full root tree (`/bin`, `/lib`, `/usr`, seed files) into `build/rootfs/` and bakes it into `disk.img` with `mke2fs -d`. The kernel mounts ext2 as `/` at boot and `panic`s if it cannot. The kernel no longer embeds or unpacks binaries (`user_blob`/`lisp_blob`/`initrd.c` are removed); KTESTs that needed an in-RAM fixture seed it themselves.

**Tech Stack:** C (freestanding aarch64 kernel), ext2 driver (`src/ext2.c`, already read+write), GNU Make, `mke2fs -d`, QEMU virtio-blk, Lisp userland, Python integration checks (QEMU serial + QMP).

**Critical constraint:** The pre-commit hook runs `make test`. Under `-DTEST_EXIT`, `kmain` runs the KTESTs and `qemu_exit()`s **before** reaching the filesystem-mount/`proc_spawn` code (`src/kmain.c:66-68`), so the boot-mount change cannot break the KTEST gate. What *can* break it: (a) the tree failing to compile, and (b) the five KTESTs that call `initrd_unpack()` directly. Every task below must leave the tree compiling with `make test` green.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `Makefile` | Build the kernel + the disk image | Stage full rootfs into `disk.img`; drop `user_blob`/`lisp_blob` from kernel `OBJ` + their recipes |
| `src/kmain.c` | Boot sequence | Mount ext2 as root; move `virtio_blk_init` up; panic on mount failure; drop `initrd_unpack` + `/disk` submount; `/disk/boots` → `/boots` |
| `src/tests.c` | In-kernel KTESTs | Replace `initrd_unpack()` fixture with a local `test_seed_fs()` helper; repurpose the initrd test |
| `src/initrd.c`, `src/initrd.h` | (was) embed+unpack userland | **Deleted** |
| `user/lisp/system.l` | Lisp OS library | `cc` link line `/disk/usr/...` → `/usr/...` |
| `tools/lm_harness.py` | QEMU integration harness | Add opt-in to reuse one scratch disk across two boots |
| `tools/persist_check.py` | New integration test | Prove a write survives a real reboot |

---

## Task 1: Install the full root filesystem onto the disk image

Stage the binaries, Lisp library, crt, and seed sources into `build/rootfs/` so `mke2fs -d` bakes them into `disk.img`. The kernel still embeds them too at this point (harmless redundancy removed in Task 4); this task only makes the disk self-sufficient.

**Files:**
- Modify: `Makefile` — the `$(BUILD)/disk.img` recipe (currently lines 130-142)

- [ ] **Step 1: Write the failing test**

Create `tools/check_rootfs_staging.sh`:

```bash
#!/usr/bin/env bash
# Verify the disk-image staging dir holds a complete root filesystem.
set -euo pipefail
cd "$(dirname "$0")/.."
make --no-print-directory build/disk.img >/dev/null
R=build/rootfs
fail=0
for f in \
  bin/init bin/lisp bin/sh bin/busybox bin/tcc bin/teapot \
  lib/bootstrap.l lib/system.l lib/frame.l lib/mycrt.o \
  hello.c hellobare.c \
  usr/include/stdio.h usr/lib/libc.a usr/lib/libtcc1.a \
  init.l test/small.txt test/big.bin
do
  if [ ! -e "$R/$f" ]; then echo "MISSING: $R/$f"; fail=1; fi
done
if [ "$fail" = 0 ]; then echo "rootfs staging OK"; else echo "rootfs staging FAILED"; exit 1; fi
```

- [ ] **Step 2: Run it to verify it fails**

Run: `chmod +x tools/check_rootfs_staging.sh && tools/check_rootfs_staging.sh`
Expected: `MISSING: build/rootfs/bin/init` (and others) → `rootfs staging FAILED`, exit 1. (Binaries/lisp not staged yet; `usr/...`, `init.l`, `test/*` already pass.)

- [ ] **Step 3: Stage the root tree in the disk-image recipe**

In `Makefile`, replace the `$(BUILD)/disk.img` recipe body. The new prerequisites ensure every artifact is built before staging. `lm.elf` is copied to both `bin/init` and `bin/lisp` (as `initrd.c` did). Edit the recipe to:

```makefile
$(BUILD)/disk.img: $(USER_ELFS) $(MUSL_ELFS) $(PREBUILT_ELFS) $(BUILD)/user/mycrt.elf \
                   $(BUILD)/user/libtcc1.a $(patsubst %,user/lisp/%.l,$(LISP_FILES)) | $(BUILD)
	rm -rf $(BUILD)/rootfs && mkdir -p $(BUILD)/rootfs/test $(BUILD)/rootfs/bin $(BUILD)/rootfs/lib
	printf '(run-bg "lisp" "-frame")\n' > $(BUILD)/rootfs/init.l
	printf 'ext2-small-file-ok\n' > $(BUILD)/rootfs/test/small.txt
	yes 0123456789ABCDEF | head -c 16384 > $(BUILD)/rootfs/test/big.bin
	# --- /bin: the userland ELFs (init == lisp == lm.elf) ---
	cp $(BUILD)/user/lm.elf $(BUILD)/rootfs/bin/init
	cp $(BUILD)/user/lm.elf $(BUILD)/rootfs/bin/lisp
	for p in $(PROGS) $(MUSL_PROGS) $(PREBUILT_PROGS); do \
	  cp $(BUILD)/user/$$p.elf $(BUILD)/rootfs/bin/$$p; done
	# --- /lib: the Lisp library + the crt tcc links against ---
	for f in $(LISP_FILES); do cp user/lisp/$$f.l $(BUILD)/rootfs/lib/$$f.l; done
	cp $(BUILD)/user/mycrt.elf $(BUILD)/rootfs/lib/mycrt.o
	# --- seed sources (were in initrd.c); persist once edited on-device ---
	printf '#include <stdio.h>\nint main(void){\n  printf("hello from tcc on myosv2: x=%%d s=%%s\\n", 42, "ok");\n  return 0;\n}\n' > $(BUILD)/rootfs/hello.c
	printf 'void puts(const char *);\nint main(void){\n  puts("hello from tcc on myosv2\\n");\n  return 0;\n}\n' > $(BUILD)/rootfs/hellobare.c
	# --- /usr: the musl sysroot (moved from /disk/usr to /usr) ---
	mkdir -p $(BUILD)/rootfs/usr/include $(BUILD)/rootfs/usr/lib
	cp -RL $(MUSL_INC)/* $(BUILD)/rootfs/usr/include/
	cp $(call print-musl,crt1.o) $(call print-musl,crti.o) \
	   $(call print-musl,crtn.o) $(call print-musl,libc.a) \
	   $(BUILD)/rootfs/usr/lib/
	cp $(BUILD)/user/libtcc1.a $(BUILD)/rootfs/usr/lib/
	dd if=/dev/zero of=$@ bs=1m count=$(DISK_MB) 2>/dev/null
	$(MKE2FS) -t ext2 -F -q -b 1024 -d $(BUILD)/rootfs $@
```

Note the doubled `%%d`/`%%s` (Make escapes `%`) and the `$$p`/`$$f` shell variables. The `lm` binary is in `$(PROGS)` and also copied as `init`/`lisp`; the loop additionally creates `/bin/lm` (harmless — matches the old initrd, which exposed `/bin/lisp` and `/bin/init` and `/bin/lm` all from `lm_elf`).

- [ ] **Step 4: Run the test to verify it passes**

Run: `rm -f build/disk.img && tools/check_rootfs_staging.sh`
Expected: `rootfs staging OK`, exit 0.

- [ ] **Step 5: Verify the kernel + KTESTs still build/pass (nothing else changed yet)**

Run: `make test`
Expected: the suite runs to `ALL TESTS PASSED` (or the existing green summary). `disk.img` rebuilt with the new layout; KTESTs unaffected.

- [ ] **Step 6: Commit**

```bash
git add Makefile tools/check_rootfs_staging.sh
git commit -m "build(fs): install full root tree (bin/lib/usr/seeds) into ext2 disk image"
```

---

## Task 2: Decouple KTESTs from `initrd_unpack`

Five KTESTs use `vfs_mount_root(ramfs_type()); initrd_unpack();` to get an in-RAM `/hello.txt`. Replace that with a self-contained `test_seed_fs()` helper so `initrd.c` can be deleted in Task 4. This is the only KTEST coupling to the removed code.

**Files:**
- Modify: `src/tests.c` — add helper; update 5 call sites + the registration table; remove `#include "initrd.h"`

- [ ] **Step 1: Add the seed helper and rewrite the initrd test (write the new test first)**

In `src/tests.c`, add this helper just above `test_initrd_unpacked` (line ~746). It mounts a fresh ramfs and writes the exact bytes the fd/syscall tests expect:

```c
// Seed a fresh in-RAM filesystem for the VFS/syscall KTESTs: a ramfs root with
// /hello.txt = "Hello, MyOSv2!\n". Replaces the old initrd_unpack() fixture now
// that the real userland lives on the ext2 disk image, not embedded in the
// kernel. Self-contained so these tests never depend on production seed code.
static void test_seed_fs(void)
{
    vfs_mount_root(ramfs_type());
    struct vnode *vn = vfs_create("/hello.txt", VN_FILE);
    struct file f = { .vnode = vn, .off = 0 };
    vfs_write(&f, "Hello, MyOSv2!\n", 15);
}
```

Then replace the body of `test_initrd_unpacked` (rename it to `test_seed_fs_provides_hello`) to test the helper:

```c
static void test_seed_fs_provides_hello(void)
{
    pmm_init(); kheap_init();
    test_seed_fs();
    struct file *f = vfs_open("/hello.txt");
    KASSERT(f != 0);
    char buf[16] = {0};
    int n = vfs_read(f, buf, 14);
    KASSERT(n == 14);
    KASSERT(bytes_eq(buf, "Hello, MyOSv2!\n", 14));
    vfs_close(f);
}
```

- [ ] **Step 2: Update the four fd-test call sites**

In `src/tests.c`, in `test_fd_open_returns_fd`, `test_fd_read_syscall`, `test_fd_open_missing`, and `test_fd_close_reuse`, replace each line:

```c
    vfs_mount_root(ramfs_type()); initrd_unpack();
```

with:

```c
    test_seed_fs();
```

(`test_seed_fs` does not call `pmm_init/kheap_init`; those lines already precede each call site, so leave them.)

- [ ] **Step 3: Update the registration table and remove the initrd include**

In the test table (line ~2926) change:

```c
    { "initrd: unpacks files",            test_initrd_unpacked },
```

to:

```c
    { "fs: seed provides /hello.txt",     test_seed_fs_provides_hello },
```

Remove `#include "initrd.h"` (line ~24) from `src/tests.c`.

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: green; the renamed test `fs: seed provides /hello.txt` and the four fd tests pass using the new helper. `initrd.c` still exists/compiles (still called by `kmain` until Task 3), so the build is intact.

- [ ] **Step 5: Commit**

```bash
git add src/tests.c
git commit -m "test(fs): seed KTEST filesystem locally instead of via initrd_unpack"
```

---

## Task 3: Mount ext2 as root and retire `/disk`

The cutover. The kernel mounts ext2 as `/`; ramfs is no longer the root; `initrd_unpack` is no longer called; `/disk` disappears (it *is* the root now), so every `/disk/...` path moves to `/...`.

**Files:**
- Modify: `src/kmain.c` — filesystem section (lines ~80-131) + include
- Modify: `user/lisp/system.l` — `cc` link line (lines ~76-80)

- [ ] **Step 1: Rewrite the kmain filesystem + block section**

`src/kmain.c` already calls `pmm_init()` then `kheap_init()` at lines 76-78 (the "--- 3. Dynamic memory ---" block) — **keep those untouched**. Replace only the block from the `// --- 4. Filesystem` comment (line 80) through the end of the disk `if/else` (line 131) with the following (it does *not* re-call `pmm_init`/`kheap_init`):

```c
    // --- 4. Block device + ext2 ROOT filesystem ---
    // The root '/' is the persistent ext2 image on the virtio-blk disk -- the
    // Linux model: the build "installs" the userland (/bin, /lib, /usr, seed
    // files) onto the image, and we mount it here. There is no ramfs root and no
    // initrd to unpack; on-device edits live on disk and survive reboots. A
    // blank/corrupt/non-ext2 disk has no root to boot, so we panic -- exactly
    // like Linux's "VFS: Unable to mount root fs".
    virtio_blk_init();
    if (!block_present()) { panic("VFS: no block device -- cannot mount root fs"); }
    vfs_mount_root(ext2_type());
    if (!vfs_root()) { panic("VFS: unable to mount root fs (ext2 mount failed)"); }
    kprintf("rootfs: / mounted (ext2)\n");

    // Persistence proof: a boot counter at /boots. It is now an ordinary file on
    // the root filesystem, so it survives reboots without any special handling.
    {
        int n = 0;
        struct file *bf = vfs_open("/boots");
        if (bf) {
            char b[16] = {0};
            int k = vfs_read(bf, b, 15);
            for (int i = 0; i < k && b[i] >= '0' && b[i] <= '9'; i++) { n = n * 10 + (b[i] - '0'); }
            vfs_close(bf);
        }
        n++;
        if (!vfs_lookup("/boots")) { vfs_create("/boots", VN_FILE); }
        struct file *wf = vfs_open("/boots");
        if (wf) {
            if (wf->vnode) { vfs_truncate(wf->vnode); }
            char b[16]; int i = 0, v = n; char t[16]; int j = 0;
            if (v == 0) { t[j++] = '0'; }
            while (v) { t[j++] = (char)('0' + v % 10); v /= 10; }
            while (j) { b[i++] = t[--j]; }
            wf->off = 0;
            vfs_write(wf, b, i);
            vfs_close(wf);
        }
        kprintf("rootfs: /boots boot count %d\n", n);
    }
```

Notes for the implementer:
- `pmm_init`/`kheap_init` stay at lines 76-78 and run exactly once, before `virtio_blk_init` — do not add or move them.
- `vfs_root()` returns the mounted root vnode (`src/vfs.c:32`); `vfs_mount_root` sets it from `fs->mount()`, which is `ext2_mount()` returning NULL on failure — hence the `!vfs_root()` check.
- Confirm `panic` is available in the kernel. Grep `src/` for an existing `panic(`/`PANIC(` and use it; if there is none, use the halt idiom already used at `kmain.c:71-72`: `kprintf(...); for(;;){ __asm__ volatile("wfi"); }`.

- [ ] **Step 2: Remove the initrd include and ramfs-root usage from kmain**

In `src/kmain.c` remove `#include "initrd.h"` (line 23). Keep `#include "ramfs.h"` only if still referenced; if nothing in `kmain.c` uses `ramfs_type()` anymore, remove that include too. `#include "ext2.h"` is already present (line 27).

- [ ] **Step 3: Move the `cc` sysroot paths in system.l from /disk to /**

In `user/lisp/system.l`, in `defun cc` (lines ~76-80), change every `/disk/usr/` to `/usr/`:

```lisp
  (run "tcc" "-nostdlib" "-static" "-Wl,-Ttext=0x8000000000"
       "-I/usr/include"
       "/usr/lib/crt1.o" "/usr/lib/crti.o" src
       "-L/usr/lib" "-lc" "/usr/lib/libtcc1.a"
       "/usr/lib/crtn.o" "-o" out))
```

Also update the docstring references to `/disk` in that function to `/` (the comment says "baked onto /disk"; change to "baked onto the ext2 root /"). `cc-bare` uses `/lib/mycrt.o` — unchanged.

- [ ] **Step 4: Verify the build + KTESTs still pass**

Run: `make test`
Expected: green. (`make test` exits before the new mount code runs, and `initrd.c` is still compiled — now unreferenced — so the build links fine.)

- [ ] **Step 5: Verify the real boot mounts ext2 root and the userland is present**

Run (boots the actual OS from a copy of the image, drives the REPL over serial):

```bash
python3 - <<'PY'
import sys; sys.path.insert(0, "tools")
from lm_harness import Qemu
q = Qemu()
try:
    assert q.expect(b"rootfs: / mounted (ext2)", 40), "root not mounted from ext2"
    assert q.expect(b"lisp> ", 40), "no REPL prompt"
    q.send_line('(ls "/bin")')
    assert q.expect(b"busybox", 10), "/bin/busybox not on root fs"
    q.send_line('(cat "/hello.c")')
    assert q.expect(b"hello from tcc", 10), "/hello.c not readable from root fs"
    print("ROOT-BOOT OK")
finally:
    q.close()
PY
```

Expected: `ROOT-BOOT OK`. (`Qemu` copies `build/disk.img` to a scratch image, so this is non-destructive. If `Qemu` lacks a `.close()`, use the harness's existing teardown method — check `tools/lm_harness.py`.)

- [ ] **Step 6: Verify on-device compile still works against the moved sysroot**

Run: `python3 tools/printf_check.py`
Expected: PASS — `cc` now links against `/usr/...` and the program prints `hello from tcc on myosv2: x=42 s=ok`. This proves the `/disk/usr` → `/usr` move end-to-end.

- [ ] **Step 7: Commit**

```bash
git add src/kmain.c user/lisp/system.l
git commit -m "feat(fs): mount ext2 as root /; retire /disk; persistent on-device edits"
```

---

## Task 4: Remove the kernel-embedded userland

Now that the userland lives on disk and no KTEST or boot path calls `initrd_unpack`, delete the embedding machinery: a smaller kernel and a single source of truth.

**Files:**
- Delete: `src/initrd.c`, `src/initrd.h`
- Modify: `Makefile` — drop `user_blob.o`/`lisp_blob.o` from `OBJ` and remove their recipes

- [ ] **Step 1: Delete the initrd source**

```bash
git rm src/initrd.c src/initrd.h
```

- [ ] **Step 2: Drop the blobs from the kernel object list**

In `Makefile`, remove these two lines from the `OBJ :=` definition (lines 23-24):

```makefile
        $(BUILD)/user_blob.o \
        $(BUILD)/lisp_blob.o
```

So `OBJ` becomes just the `src/*.c` + `src/*.S` patterns. Update the comment on line 20 (`# user_blob.o = embedded ...`) to note the userland now lives on the disk image.

- [ ] **Step 3: Remove the blob recipes**

In `Makefile`, delete the `$(BUILD)/user_blob.c`, `$(BUILD)/user_blob.o`, `$(BUILD)/lisp_blob.c`, and `$(BUILD)/lisp_blob.o` rules (the block at lines ~251-269). The `$(USER_ELFS)`/`$(MUSL_ELFS)`/`$(PREBUILT_ELFS)`/`mycrt.elf`/`libtcc1.a`/`LISP_FILES` definitions and their `.elf` build rules stay — they are now prerequisites of `disk.img` (wired in Task 1).

- [ ] **Step 4: Verify a clean build links without the blobs and KTESTs pass**

Run: `make clean && make test`
Expected: the kernel links with no `user_blob`/`lisp_blob`/`initrd` symbols, and the suite is green. (If the link fails with an undefined `*_elf`/`initrd_unpack` symbol, a reference was missed — grep `src/` for it.)

- [ ] **Step 5: Sanity-check no dangling references**

Run: `grep -rn "initrd\|user_blob\|lisp_blob" src/ Makefile`
Expected: no matches (or only historical comments you choose to leave). Fix any live reference.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "build(fs): drop kernel-embedded userland (user_blob/lisp_blob/initrd) -- now on disk"
```

---

## Task 5: Prove persistence across a real reboot

The headline behavior, as a TDD integration test: write a file on one boot, reboot the **same** disk, read it back. The current harness copies a pristine image per run, so add an opt-in to reuse one scratch image across two boots.

**Files:**
- Modify: `tools/lm_harness.py` — let `Qemu` reuse an existing scratch disk instead of re-copying
- Create: `tools/persist_check.py`

- [ ] **Step 1: Inspect the harness scratch-disk setup**

Read `tools/lm_harness.py` around `SCRATCH_DISK` / `_BUILT_DISK` (lines ~36-50) and the `Qemu.__init__` that copies the image. Identify the exact line that does `shutil.copy(_BUILT_DISK, SCRATCH_DISK)` (or equivalent).

- [ ] **Step 2: Add a `fresh` parameter to `Qemu`**

In `tools/lm_harness.py`, change the `Qemu` constructor so the image copy is conditional. Concretely, find the copy call in `__init__` and guard it:

```python
def __init__(self, ..., fresh=True):
    ...
    if fresh:
        shutil.copy(_BUILT_DISK, SCRATCH_DISK)   # pristine per run (existing behavior)
    # when fresh=False, reuse whatever is already at SCRATCH_DISK (persistence tests)
```

Keep the default `fresh=True` so every existing check is unaffected. (Match the real parameter list and copy mechanism you found in Step 1; the key change is the `if fresh:` guard.)

- [ ] **Step 3: Write the persistence test**

Create `tools/persist_check.py`:

```python
#!/usr/bin/env python3
"""
persist_check.py -- the headline behavior of the ext2-root work: a file written
on one boot is still there after a reboot of the SAME disk.

Boot 1 (fresh image): write a sentinel to /persist-test.txt via the REPL.
Boot 2 (REUSE the same scratch image, fresh=False): read it back -- the bytes
must survive, proving the root filesystem is genuinely persistent.
"""
import sys
sys.path.insert(0, "tools")
from lm_harness import Qemu

SENTINEL = "PERSISTED-OK-7351"


def main() -> int:
    # Boot 1: fresh image, write the file.
    q = Qemu(fresh=True)
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: boot 1 no REPL"); return 1
        q.send_line('(let ((fd (creat "/persist-test.txt")))'
                     ' (fd-write fd "%s") (close fd))' % SENTINEL)
        # Give the write a moment to flush through the ext2 write path.
        if not q.expect(b"lisp> ", 10):
            print("FAIL: write did not return to prompt"); return 1
    finally:
        q.close()

    # Boot 2: REUSE the same scratch disk; the file must still be there.
    q = Qemu(fresh=False)
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: boot 2 no REPL"); return 1
        q.send_line('(cat "/persist-test.txt")')
        if not q.expect(SENTINEL.encode(), 10):
            print("FAIL: sentinel not found after reboot -- root not persistent"); return 1
    finally:
        q.close()

    print("PERSIST OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

(If `Qemu` exposes teardown as something other than `.close()`, match it — check Step 1's reading. If `creat`/`fd-write`/`cat` names differ, confirm against `user/lisp/system.l` and `user/lm_sys.c`; this plan uses the names verified there.)

- [ ] **Step 4: Run it to verify it passes (the behavior is real)**

Run: `python3 tools/persist_check.py`
Expected: `PERSIST OK`. Before this whole plan, the same test on a ramfs root would fail at boot 2 (`sentinel not found`); now it survives.

- [ ] **Step 5: Confirm the image is still ext2-clean after the writes**

Run: `e2fsck -fn /tmp/myosv2-test-disk-*.img`
Expected: clean (no errors). (Use the actual `SCRATCH_DISK` path printed/derived by the harness; it embeds the PID.)

- [ ] **Step 6: Commit**

```bash
git add tools/lm_harness.py tools/persist_check.py
git commit -m "test(fs): persist_check -- a file written survives a real reboot of the ext2 root"
```

---

## Task 6: Refresh docs (README + roadmap)

Per the standing "keep README current" rule, reflect the new persistent root.

**Files:**
- Modify: `README.md` — filesystem capability summary
- Modify: any roadmap/handoff doc that describes `/disk` as the only persistent FS (e.g. `docs/` roadmap)

- [ ] **Step 1: Update the README filesystem section**

Find the README passage describing the filesystem (search for `ramfs`, `/disk`, or `ext2`). Update it to state: the root `/` is a persistent ext2 image installed at build time (`/bin`, `/lib`, `/usr`, seed sources); on-device edits and new files survive reboots; the kernel no longer embeds the userland. Remove claims that `/` is ramfs or that only `/disk` persists.

- [ ] **Step 2: Update the roadmap/handoff note**

Search `docs/` for references to ramfs-root / `/disk`-only persistence and update them to match. If a phase/roadmap doc tracks this work, mark the ext2-root milestone done.

- [ ] **Step 3: Verify nothing stale remains**

Run: `grep -rn "ramfs.*root\|/disk/usr\|only.*persist" README.md docs/ | grep -iv spec`
Expected: no misleading matches (the design spec itself may mention `/disk` historically — that is fine).

- [ ] **Step 4: Commit**

```bash
git add README.md docs/
git commit -m "docs(fs): README + roadmap reflect persistent ext2 root"
```

---

## Final verification (after all tasks)

- [ ] `make clean && make test` → green (compile + KTESTs).
- [ ] `python3 tools/persist_check.py` → `PERSIST OK`.
- [ ] `python3 tools/printf_check.py` → on-device printf against `/usr` libc works.
- [ ] `python3 tools/findfile_check.py` and `python3 tools/frameedit_check.py` → create-on-save still works (now persistent).
- [ ] `grep -rn "initrd\|user_blob\|lisp_blob" src/ Makefile` → no live references.
- [ ] Dispatch a final whole-implementation code review, then `superpowers:finishing-a-development-branch`.
