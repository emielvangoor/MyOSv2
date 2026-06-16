CROSS   := aarch64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD   := build
TARGET  := $(BUILD)/kernel.elf

# -ffreestanding: no hosted libc assumptions
# -nostdlib/-nostartfiles: we provide our own _start, no C runtime
# -mgeneral-regs-only: don't touch FP/SIMD regs (not set up yet)
# -MMD -MP: generate header dependency files
# -g: emit DWARF debug info so GDB can do source-level stepping
CFLAGS  := -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only \
           -Wall -Wextra -O2 -g -ffunction-sections -MMD -MP $(EXTRA_CFLAGS)
LDFLAGS := -nostdlib -nostartfiles -T linker.ld -Wl,--gc-sections

CSRC := $(wildcard src/*.c)
ASRC := $(wildcard src/*.S)
# The full userland (every ELF + all Lisp .l library files) now lives on the
# ext2 disk image (see the disk.img recipe); the kernel reads programs from disk
# at runtime via as_create_elf() in proc.c / sched.c.  user_blob.o is the ONE
# exception: it embeds ONLY sh.elf as a C byte array (sh_elf / sh_elf_len) so
# the VM / address-space self-tests (src/tests.c, src/vm.c) can load a real ELF
# without touching the disk.  It is pure test scaffolding, NOT runtime userland.
OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) \
        $(patsubst src/%.S,$(BUILD)/%.o,$(ASRC)) \
        $(BUILD)/user_blob.o
DEP  := $(OBJ:.o=.d)

# User programs are separate ELF64 executables linked at USER_CODE_VA. They are
# staged into $(BUILD)/rootfs/bin/ and baked onto the ext2 disk image (see the
# disk.img recipe) -- they are NOT embedded in the kernel. The kernel's ELF
# loader maps their segments at exec time, reading the ELF from disk.
PROGS       := sh true false hello mtest shmtest wc loop catch ping dnsq http httpd polldemo lm evtest gfxtest surftest fptest teapot
# The Lisp library: .l files copied into $(BUILD)/rootfs/lib/ and baked onto the
# ext2 disk image; /bin/lisp loads them from /lib at startup.
LISP_FILES  := bootstrap system modes fr-repl fr-edit fr-modes fr-keys fr-files fr-mini fr-help frame
USER_COMMON := user/crt0.S user/ulib.c
USER_ELFS   := $(patsubst %,$(BUILD)/user/%.elf,$(PROGS))

# Real Linux binaries, built with the musl cross-compiler (-static -no-pie so
# the loader needs no relocations) and staged onto the ext2 disk image alongside
# the native programs. The whole point of Phase 28: these run on the migrated ABI.
MUSL_CC     := aarch64-linux-musl-gcc
MUSL_PROGS  := mhello mmalloc mfork mfile
MUSL_ELFS   := $(patsubst %,$(BUILD)/user/%.elf,$(MUSL_PROGS))

# Prebuilt static-musl binaries staged onto the ext2 disk image as-is (built from
# source with CONFIG_STATIC + -Wl,-Ttext-segment=0x8000000000; see user/musl/*.bin).
# busybox is the forcing function for the long syscall tail.
PREBUILT_PROGS := busybox tcc
PREBUILT_ELFS  := $(patsubst %,$(BUILD)/user/%.elf,$(PREBUILT_PROGS))
# -z max-page-size=4096: align segments to 4 KiB (our page size) instead of the
# AArch64 default 64 KiB, so PT_LOAD vaddrs/offsets are page-aligned and small.
# No -mgeneral-regs-only here (unlike CFLAGS): the FPU is enabled and its
# state is context-switched, so user programs may use floats and NEON.
USER_CFLAGS := -ffreestanding -nostdlib -nostartfiles -Wall -O2 \
               -Wl,-z,max-page-size=0x1000

QEMU       := qemu-system-aarch64
# -display none: no graphical window.
# Serial is a plain stdio chardev with signal=off: that hands Ctrl-C to the GUEST
# (so our shell can interrupt a running program) instead of sending SIGINT to the
# QEMU process and killing it -- which is what plain `-serial stdio` does. We do
# NOT multiplex the monitor onto it (mux=on), because the mux's keyboard "focus"
# can default to the monitor in some terminals and then no input reaches the OS.
# To quit QEMU: close the terminal, or `pkill qemu-system-aarch64` from another.
# -m 256M: fix the RAM size so the page allocator knows where RAM ends (0x50000000).
# A virtio-blk disk on a virtio-mmio transport (modern, non-legacy), backed by a
# raw image file -- the OS reads/writes its 512-byte sectors.
QEMU_DISK  = -global virtio-mmio.force-legacy=false \
              -drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
              -device virtio-blk-device,drive=hd0
DISK_IMG   = $(BUILD)/disk.img
# QEMU user-mode networking: a virtual LAN (gateway 10.0.2.2, guest 10.0.2.15)
# with a built-in ARP/ICMP/DHCP responder -- no host setup needed.
QEMU_NET   := -netdev user,id=net0 -device virtio-net-device,netdev=net0
# Interactive runs additionally forward host ports into the guest:
#   8080 -> 8080  /bin/httpd        (`httpd`, then `curl http://localhost:8080/`)
#   7777 -> 7777  Lisp network REPL (`lisp -serve`, then connect from Emacs --
#                 see user/lisp/lm-mode.el). 7777 instead of the classic 7000
#                 because macOS's AirPlay Receiver listens on 7000.
# These are NOT used by `make test` -- binding a host port would make the test
# suite fail whenever it is busy (a server already running, overlapping runs).
QEMU_NET_RUN := -netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=tcp::7777-:7777 \
                -device virtio-net-device,netdev=net0
QEMU_SERIAL := -chardev stdio,id=ch0,signal=off -serial chardev:ch0
# Keyboard + absolute-pointer tablet for the graphical machine (Phase 25). The
# tablet reports absolute coordinates, so QEMU never grabs the host mouse.
# Part of the base flags so `make test` sees the devices too (KTEST drives them).
QEMU_INPUT := -device virtio-keyboard-device -device virtio-tablet-device
# The display device (Phase 25.2). With -display none QEMU still renders the
# scanout offscreen, so KTEST can drive the device and QMP can screendump it.
QEMU_GPU   := -device virtio-gpu-device
# Base flags WITHOUT networking; run/test/debug each add the net variant they want.
# Recursively expanded (=, not :=): run/run-gui override DISK_IMG per target,
# which must reach QEMU_DISK at recipe time, not be baked in at parse time.
QEMU_FLAGS = -machine virt -cpu cortex-a72 -m 256M -display none $(QEMU_SERIAL) \
              $(QEMU_INPUT) $(QEMU_GPU) -kernel $(TARGET) $(QEMU_DISK)

.PHONY: all run debug gdb clean objdump compile_commands test
all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

# The ROOT filesystem is a real ext2 image, built ON THE HOST with mke2fs's
# `-d` (populate from a directory) so it ships pre-loaded with the full userland
# (/bin, /lib, /usr), /init.l, and the KTEST fixtures -- the kernel mounts it as
# / and reads what the host laid down. One ext2 block = 1024 bytes (forced
# -b 1024 => 2 disk sectors per block, matching
# the driver). MKE2FS is overridable for other hosts.
MKE2FS  ?= /opt/homebrew/share/android-commandlinetools/platform-tools/mke2fs
DISK_MB := 64

# The musl sysroot we bake onto /disk so the on-device TinyCC (/bin/tcc) can
# compile programs that #include <stdio.h> and link against a real libc. We pull
# the headers + the static-link objects (crt1/crti/crtn + libc.a) straight out
# of the cross toolchain. -print-sysroot / -print-file-name resolve the exact
# host paths (they vary: lib/ vs lib64/, include/ vs usr/include/), so we never
# hard-code Homebrew's Cellar layout.
MUSL_SYSROOT := $(shell $(MUSL_CC) -print-sysroot)
# Headers ship under usr/include on this toolchain; fall back to include if a
# host puts them at the sysroot root.
MUSL_INC := $(firstword $(wildcard $(MUSL_SYSROOT)/usr/include $(MUSL_SYSROOT)/include))
# Resolve the static-link objects by name -- they may live in lib/ or lib64/.
print-musl = $(shell $(MUSL_CC) -print-file-name=$(1))

# Stage build/rootfs/ then bake it into an ext2 image. The staging dir becomes
# the complete persistent root filesystem:
#   /init.l         -- boot script: launches the Lisp frame
#   /test/          -- deterministic fixtures for the ext2 read KTESTs (big.bin
#                      is 16 KiB, forcing single-indirect block reads)
#   /bin/           -- all userland ELFs; init == lisp == lm.elf (three names,
#                      one binary: the kernel exec()s /bin/init at boot, the
#                      shell runs /bin/lisp for a REPL, /bin/lm by legacy name)
#   /lib/           -- Lisp source library (.l files) + mycrt.o (the minimal
#                      C runtime tcc links user programs against)
#   /hello.c /hellobare.c -- seed C sources; persist once edited on-device
#   /usr/include/   -- musl headers (symlinks dereferenced: our ext2 driver has
#                      no symlink support, so the image must hold real files)
#   /usr/lib/       -- musl crt1/crti/crtn/libc.a + libtcc1.a (TCC's compiler-
#                      support runtime; tcc-compiled programs need both)
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
	# --- seed C sources; persist once edited on-device ---
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

# The PERSISTENT disk for interactive runs: lives at the repo root (gitignored)
# so `make clean` never destroys it -- anything the machine writes survives. It
# is just a copy of the freshly built ext2 image (which already boots the frame
# via /init.l); existing disks are left alone.
DISK := disk.img
$(DISK): $(BUILD)/disk.img
	cp $(BUILD)/disk.img $@

# Rebuild the persistent disk from scratch (re-bakes the ext2 image, re-seeds
# /init.l). Destroys any files the machine wrote.
.PHONY: fresh-disk
fresh-disk:
	rm -f $(DISK) $(BUILD)/disk.img
	$(MAKE) --no-print-directory $(DISK)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Build each user program as an ELF executable: crt0 + ulib + <prog>.c.
$(BUILD)/user/%.elf: user/%.c $(USER_COMMON) user/user.ld user/ulib.h user/syscalls.h | $(BUILD)
	mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -T user/user.ld -o $@ $(USER_COMMON) user/$*.c

# /bin/lisp is special: it links the shared Lisp core (the SAME src/lm_core.c the
# kernel compiles for its tests) plus the freestanding setjmp, so it needs extra
# sources and -Isrc to find lm.h. lm_sys.c (the syscall primitives) is USER-ONLY:
# the kernel build of the core must never see it. This explicit rule overrides
# the generic one above for lm.elf.
LM_CORE := src/lm_core.c src/lm_jmp.S src/rd_core.c
$(BUILD)/user/lm.elf: user/lm.c user/lm_sys.c user/lm_sys.h user/lm_gfx.c $(LM_CORE) src/lm.h src/rd.h $(USER_COMMON) user/user.ld user/ulib.h user/syscalls.h | $(BUILD)
	mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -Isrc -T user/user.ld -o $@ $(USER_COMMON) $(LM_CORE) user/lm.c user/lm_sys.c user/lm_gfx.c

# TinyGL (vendored, user/tinygl/): software OpenGL 1.x compiled freestanding
# against the shim headers; tgl_rt.c supplies its libc/math needs over ulib.
TGL_SRC  := $(wildcard user/tinygl/src/*.c) user/tinygl/tgl_rt.c
TGL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(TGL_SRC))
TGL_INC  := -Iuser/tinygl/shim -Iuser/tinygl/include -Iuser
$(BUILD)/user/tinygl/%.o: user/tinygl/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -w $(TGL_INC) -c $< -o $@

# /bin/teapot: TinyGL + the Newell patches, rendering into a surface buffer.
$(BUILD)/user/teapot.elf: user/teapot.c user/teapot_data.h $(TGL_OBJS) $(USER_COMMON) user/user.ld | $(BUILD)
	mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) $(TGL_INC) -T user/user.ld -o $@ $(USER_COMMON) user/teapot.c $(TGL_OBJS)

# Build a musl program (real Linux binary) for the ext2 disk image. We link it
# into the CLEAN user VA range (0x80_0000_0000+, l0 index >= 1) instead of musl's
# default
# 0x400000, which in our address space falls under l0[0] -- the shared kernel
# identity map (block descriptors) -- where a user page can't be inserted.
# (Running UNMODIFIED 0x400000 binaries would instead need block-splitting in
# the loader; we recompile our own programs, so we just relink them high.)
MUSL_TEXT := 0x8000000000
$(BUILD)/user/%.elf: user/musl/%.c | $(BUILD)
	mkdir -p $(BUILD)/user
	$(MUSL_CC) -static -no-pie -Os -Wl,-Ttext-segment=$(MUSL_TEXT) -o $@ $<

# Prebuilt binaries: just copy them into place for the blob.
$(BUILD)/user/%.elf: user/musl/%.bin | $(BUILD)
	mkdir -p $(BUILD)/user
	cp $< $@

# A tiny gcc-built C runtime (_start + syscall stubs) that the on-device TCC
# links user programs against -- so `tcc hello.c /lib/mycrt.o` produces a
# runnable static ELF without a full libc sysroot. Built -c (relocatable .o);
# named .elf only so the blob step picks it up uniformly.
$(BUILD)/user/mycrt.elf: user/musl/mycrt.S | $(BUILD)
	mkdir -p $(BUILD)/user
	$(MUSL_CC) -c -o $@ $<

# libtcc1.a -- TinyCC's compiler-support runtime (the helpers tcc's codegen
# *calls*: 64-bit division/modulo on a 32-bit-ish target, varargs glue,
# __builtin_* fallbacks, atomics, alloca). A program tcc compiles can reference
# these even if the .c never names them, so the link line must offer the archive
# or the link fails with "undefined symbol". tcc normally builds this with
# ITSELF; we can't run tcc on the host, so we compile the SAME arm64 runtime
# sources (lib/Makefile's OBJ-arm64 set) with the cross musl-gcc instead -- the
# resulting objects are ABI-compatible static arm64 code. The source lives at
# $(TCC_SRC) (cloned + arm64-svc-patched in an earlier phase); overridable.
TCC_SRC   ?= /tmp/tinycc
# OBJ-arm64 from tcc's lib/Makefile is: lib-arm64 + the COMMON set + armflush +
# dsohandle. We OMIT armflush.c: it calls __arm64_clear_cache, which is not a
# real function but a code-builtin tcc emits inline (tccgen.c) -- so gcc can't
# compile it, and it only matters for self-modifying code we never run. The .S
# files are GNU-as assembly; musl-gcc drives the assembler.
TCC1_SRCS := lib-arm64.c stdatomic.c builtin.c dsohandle.c \
             atomic.S alloca.S alloca-bt.S
TCC1_OBJS := $(patsubst %,$(BUILD)/user/libtcc1/%.o,$(basename $(TCC1_SRCS)))
# tcc's runtime sources #include "../tcc.h"/config.h and use -DTCC_TARGET_ARM64.
TCC1_CFLAGS := -c -O2 -Wall -I$(TCC_SRC) -DTCC_TARGET_ARM64 -DCONFIG_TCC_STATIC \
               -fno-stack-protector -funwind-tables

$(BUILD)/user/libtcc1/%.o: $(TCC_SRC)/lib/%.c | $(BUILD)
	mkdir -p $(BUILD)/user/libtcc1
	$(MUSL_CC) $(TCC1_CFLAGS) -o $@ $<

$(BUILD)/user/libtcc1/%.o: $(TCC_SRC)/lib/%.S | $(BUILD)
	mkdir -p $(BUILD)/user/libtcc1
	$(MUSL_CC) $(TCC1_CFLAGS) -o $@ $<

$(BUILD)/user/libtcc1.a: $(TCC1_OBJS)
	aarch64-linux-musl-ar rcs $@ $(TCC1_OBJS)

# The kernel embeds exactly ONE program ELF -- /bin/sh -- as a C byte array
# (sh_elf / sh_elf_len). It is NOT runtime userland: it is a sample ELF the
# VM/address-space self-tests load (as_create()/as_create_elf() in src/tests.c)
# to exercise the loader without a disk. The real userland is staged onto the
# ext2 disk image (see the disk.img recipe); production loading reads ELFs from
# disk via as_create_elf(). xxd run from build/user derives the C symbol from the
# filename (non-alphanumerics -> '_'), so sh.elf -> `sh_elf`/`sh_elf_len` -- the
# exact externs in src/vm.c and src/tests.c. Renaming the file breaks those.
$(BUILD)/user_blob.c: $(BUILD)/user/sh.elf
	cd $(BUILD)/user && : > ../user_blob.c && xxd -i sh.elf >> ../user_blob.c

$(BUILD)/user_blob.o: $(BUILD)/user_blob.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ) linker.ld
	$(CC) $(LDFLAGS) $(OBJ) -o $@

# Run in the terminal. Ctrl-C goes to the guest shell; quit QEMU by closing the
# terminal (or `pkill qemu-system-aarch64`).
run: DISK_IMG = $(DISK)
run: $(TARGET) $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET_RUN)

# Run WITH a display window (the graphical Lisp machine, Phase 25): the same
# flags minus -display none. Serial stays in the terminal; the QEMU window
# shows the scanout and grabs keyboard+tablet input when focused.
# We do NOT pass zoom-to-fit: with it on, cocoa opens a small default window
# and scales the guest DOWN into it (the "tiny window" bug); without it the
# window opens at the scanout's native size (2560x1440, see src/gfx.h). Add
# full-screen=on if you want it to fill the display. Override on other hosts:
#   make run-gui QEMU_DISPLAY=gtk,zoom-to-fit=on
QEMU_DISPLAY ?= cocoa
run-gui: DISK_IMG = $(DISK)
run-gui: $(TARGET) $(DISK)
	$(QEMU) $(filter-out -display none,$(QEMU_FLAGS)) -display $(QEMU_DISPLAY) $(QEMU_NET_RUN)

# Run the self-tests and return a shell exit code (0 = all passed). Builds a
# test kernel with -DTEST_EXIT (which exits QEMU via semihosting), runs it under
# -semihosting, then cleans so the flag never leaks into a normal `make run`.
test:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory EXTRA_CFLAGS=-DTEST_EXIT $(TARGET) $(BUILD)/disk.img
	@echo "--- running self-tests in QEMU ---"
	@$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) -semihosting; status=$$?; \
	  $(MAKE) --no-print-directory clean >/dev/null; \
	  echo "make test exit code: $$status"; \
	  exit $$status

# Boot frozen, exposing the GDB stub on :1234
debug: $(TARGET) $(BUILD)/disk.img
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) -S -s

# Attach GDB (uses .gdbinit). Run in a second terminal after `make debug`.
gdb:
	$(CROSS)gdb $(TARGET)

objdump: $(TARGET)
	$(OBJDUMP) -d $(TARGET) | less

clean:
	rm -rf $(BUILD) compile_commands.json

# Generate compile_commands.json for clangd (editor IntelliSense).
compile_commands:
	@printf '[\n' > compile_commands.json
	@first=1; for f in $(CSRC); do \
	  [ $$first -eq 1 ] || printf ',\n' >> compile_commands.json; first=0; \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
	    "$(CURDIR)" "$(CURDIR)/$$f" "$(CC)" "$(CFLAGS)" "$$f" >> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json
	@echo "wrote compile_commands.json"

-include $(DEP)
