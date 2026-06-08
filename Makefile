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
OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) \
        $(patsubst src/%.S,$(BUILD)/%.o,$(ASRC)) \
        $(BUILD)/user_blob.o          # the embedded user program
DEP  := $(OBJ:.o=.d)

# User programs are separate ELF64 executables linked at USER_CODE_VA, each
# embedded into the kernel image as a C byte array (<prog>_elf / <prog>_elf_len)
# and unpacked into /bin by the initrd. The kernel's ELF loader maps their
# segments at load/exec time.
PROGS       := sh true false hello mtest shmtest wc loop catch ping dnsq
USER_COMMON := user/crt0.S user/ulib.c
USER_ELFS   := $(patsubst %,$(BUILD)/user/%.elf,$(PROGS))
# -z max-page-size=4096: align segments to 4 KiB (our page size) instead of the
# AArch64 default 64 KiB, so PT_LOAD vaddrs/offsets are page-aligned and small.
USER_CFLAGS := -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only -Wall -O2 \
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
QEMU_DISK  := -global virtio-mmio.force-legacy=false \
              -drive file=$(BUILD)/disk.img,if=none,format=raw,id=hd0 \
              -device virtio-blk-device,drive=hd0
# QEMU user-mode networking: a virtual LAN (gateway 10.0.2.2, guest 10.0.2.15)
# with a built-in ARP/ICMP/DHCP responder -- no host setup needed.
QEMU_NET   := -netdev user,id=net0 -device virtio-net-device,netdev=net0
QEMU_SERIAL := -chardev stdio,id=ch0,signal=off -serial chardev:ch0
QEMU_FLAGS := -machine virt -cpu cortex-a72 -m 256M -display none $(QEMU_SERIAL) \
              -kernel $(TARGET) $(QEMU_DISK) $(QEMU_NET)

.PHONY: all run debug gdb clean objdump compile_commands test
all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

# A 4 MiB raw disk image for the virtio-blk device (created once if missing).
$(BUILD)/disk.img: | $(BUILD)
	dd if=/dev/zero of=$@ bs=1m count=4 2>/dev/null

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Build each user program as an ELF executable: crt0 + ulib + <prog>.c.
$(BUILD)/user/%.elf: user/%.c $(USER_COMMON) user/user.ld user/ulib.h user/syscalls.h | $(BUILD)
	mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -T user/user.ld -o $@ $(USER_COMMON) user/$*.c

# Embed every program ELF as a C byte array (<prog>_elf / <prog>_elf_len).
$(BUILD)/user_blob.c: $(USER_ELFS)
	cd $(BUILD)/user && : > ../user_blob.c && \
	  for p in $(PROGS); do xxd -i $$p.elf >> ../user_blob.c; done

$(BUILD)/user_blob.o: $(BUILD)/user_blob.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ) linker.ld
	$(CC) $(LDFLAGS) $(OBJ) -o $@

# Run in the terminal. Ctrl-C goes to the guest shell; quit QEMU by closing the
# terminal (or `pkill qemu-system-aarch64`).
run: $(TARGET) $(BUILD)/disk.img
	$(QEMU) $(QEMU_FLAGS)

# Run the self-tests and return a shell exit code (0 = all passed). Builds a
# test kernel with -DTEST_EXIT (which exits QEMU via semihosting), runs it under
# -semihosting, then cleans so the flag never leaks into a normal `make run`.
test:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory EXTRA_CFLAGS=-DTEST_EXIT $(TARGET) $(BUILD)/disk.img
	@echo "--- running self-tests in QEMU ---"
	@$(QEMU) $(QEMU_FLAGS) -semihosting; status=$$?; \
	  $(MAKE) --no-print-directory clean >/dev/null; \
	  echo "make test exit code: $$status"; \
	  exit $$status

# Boot frozen, exposing the GDB stub on :1234
debug: $(TARGET)
	$(QEMU) $(QEMU_FLAGS) -S -s

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
