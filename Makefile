# VortexOS build. Uses the host gcc in 32-bit freestanding mode by default;
# set CROSS=i686-elf- to use a proper cross toolchain.
CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
BUILD   := build
KERNEL  := $(BUILD)/vortex.bin
ISO     := $(BUILD)/vortex.iso

CFLAGS  := -m32 -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
           -mgeneral-regs-only -mno-red-zone -Iinclude -MMD -MP
ASFLAGS := -m32 -g
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

SRCS := $(wildcard boot/*.S) $(wildcard kernel/*.c)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

.PHONY: all run iso run-iso test clean
all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# Boot directly in QEMU (it understands Multiboot kernels).
run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -serial stdio

# Bootable CD image (needs grub-mkrescue + xorriso).
iso: $(ISO)
$(ISO): $(KERNEL) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/vortex.bin
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso

run-iso: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio

# Smoke test without QEMU: boots the kernel in the Unicorn CPU emulator
# (pip install unicorn) and types a few shell commands.
test: $(KERNEL)
	python3 tools/emu_test.py $(KERNEL)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
