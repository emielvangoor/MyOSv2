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
           -Wall -Wextra -O2 -g -ffunction-sections -MMD -MP
LDFLAGS := -nostdlib -nostartfiles -T linker.ld -Wl,--gc-sections

CSRC := $(wildcard src/*.c)
ASRC := $(wildcard src/*.S)
OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) \
        $(patsubst src/%.S,$(BUILD)/%.o,$(ASRC))
DEP  := $(OBJ:.o=.d)

QEMU       := qemu-system-aarch64
# -display none: no graphical window. -serial stdio: serial to terminal AND
# lets Ctrl-C (SIGINT) terminate QEMU normally (unlike -nographic).
# -m 256M: fix the RAM size so the page allocator knows where RAM ends (0x50000000).
QEMU_FLAGS := -machine virt -cpu cortex-a72 -m 256M -display none -serial stdio -kernel $(TARGET)

.PHONY: all run debug gdb clean objdump compile_commands
all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ) linker.ld
	$(CC) $(LDFLAGS) $(OBJ) -o $@

# Run in the terminal. Quit QEMU with: Ctrl-C
run: $(TARGET)
	$(QEMU) $(QEMU_FLAGS)

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
