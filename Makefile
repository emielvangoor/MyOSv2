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
PROGS       := sh true false hello mtest
USER_COMMON := user/crt0.S user/ulib.c
USER_ELFS   := $(patsubst %,$(BUILD)/user/%.elf,$(PROGS))
# -z max-page-size=4096: align segments to 4 KiB (our page size) instead of the
# AArch64 default 64 KiB, so PT_LOAD vaddrs/offsets are page-aligned and small.
USER_CFLAGS := -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only -Wall -O2 \
               -Wl,-z,max-page-size=0x1000

QEMU       := qemu-system-aarch64
# -display none: no graphical window. -serial stdio: serial to terminal AND
# lets Ctrl-C (SIGINT) terminate QEMU normally (unlike -nographic).
# -m 256M: fix the RAM size so the page allocator knows where RAM ends (0x50000000).
QEMU_FLAGS := -machine virt -cpu cortex-a72 -m 256M -display none -serial stdio -kernel $(TARGET)

.PHONY: all run debug gdb clean objdump compile_commands test
all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

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

# Run in the terminal. Quit QEMU with: Ctrl-C
run: $(TARGET)
	$(QEMU) $(QEMU_FLAGS)

# Run the self-tests and return a shell exit code (0 = all passed). Builds a
# test kernel with -DTEST_EXIT (which exits QEMU via semihosting), runs it under
# -semihosting, then cleans so the flag never leaks into a normal `make run`.
test:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory EXTRA_CFLAGS=-DTEST_EXIT $(TARGET)
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
