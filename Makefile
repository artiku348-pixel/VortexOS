# VortexOS build. Uses the host gcc in 32-bit freestanding mode by default;
# set CROSS=i686-elf- to use a proper cross toolchain.
CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AR      := $(CROSS)ar
BUILD   := build
KERNEL  := $(BUILD)/vortex.bin
INITRD  := $(BUILD)/initrd.tar
ISO     := $(BUILD)/vortex.iso
QEMU    ?= qemu-system-i386
DISK    := $(BUILD)/disk.img
QEMUFLAGS ?= -m 128 -vga std -serial stdio -drive file=$(DISK),format=raw,if=ide,index=0

# ---- kernel ---------------------------------------------------------------
CFLAGS  := -m32 -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
           -mgeneral-regs-only -mno-red-zone -Iinclude -Isdk/include -MMD -MP
ASFLAGS := -m32 -g
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

SRCS := $(wildcard boot/*.S) $(wildcard kernel/*.c)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

# ---- applications (SDK) ---------------------------------------------------
# Position-independent, freestanding ELF32 programs linked against libvx.
# apps/bin/NAME.c -> /bin/NAME (console), apps/gui/NAME.c -> /apps/NAME (GUI).
APP_CFLAGS  := -m32 -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
               -fno-stack-protector -fPIE -fvisibility=hidden \
               -fno-asynchronous-unwind-tables -mgeneral-regs-only \
               -Isdk/include -MMD -MP
APP_LDFLAGS := -m elf_i386 -pie --no-dynamic-linker -nostdlib -e _start \
               -z noexecstack -z norelro -z noseparate-code
LIBVX       := $(BUILD)/sdk/libvx.a

BIN_APPS := $(patsubst apps/bin/%.c,%,$(wildcard apps/bin/*.c))
GUI_APPS := $(patsubst apps/gui/%.c,%,$(wildcard apps/gui/*.c))
APP_ELFS := $(patsubst %,$(BUILD)/apps/bin/%,$(BIN_APPS)) \
            $(patsubst %,$(BUILD)/apps/gui/%,$(GUI_APPS))
ROOTFS_FILES := $(shell find rootfs -type f)

.PHONY: all apps run iso run-iso test clean FORCE disk
all: $(KERNEL) $(INITRD) $(DISK)
FORCE:
apps: $(APP_ELFS)

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/sdk/%.o: sdk/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(LIBVX): $(BUILD)/sdk/libvx.o
	$(AR) rcs $@ $^

$(BUILD)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/apps/%: $(BUILD)/apps/%.o $(LIBVX)
	$(LD) $(APP_LDFLAGS) -o $@ $< $(LIBVX)

# The initrd is a ustar archive of rootfs/ plus the compiled applications;
# the kernel unpacks it into the in-memory filesystem at boot.
$(INITRD): $(APP_ELFS) $(ROOTFS_FILES) FORCE
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/apps
	cp -r rootfs/. $(BUILD)/rootfs/
	$(if $(BIN_APPS),cp $(patsubst %,$(BUILD)/apps/bin/%,$(BIN_APPS)) $(BUILD)/rootfs/bin/)
	$(if $(GUI_APPS),cp $(patsubst %,$(BUILD)/apps/gui/%,$(GUI_APPS)) $(BUILD)/rootfs/apps/)
	tar --format=ustar --owner=0 --group=0 -cf $@ -C $(BUILD)/rootfs .

# The hard disk: a 16 MiB FAT16 image seeded from diskfs/ and mounted at
# /disk. It is created only once, so files saved by the OS are kept between
# runs; `make disk` starts over with a fresh image.
$(DISK):
	@mkdir -p $(BUILD)
	python3 tools/mkfat.py create $@ diskfs 16

disk:
	@mkdir -p $(BUILD)
	python3 tools/mkfat.py create $(DISK) diskfs 16

# Boot directly in QEMU (it understands Multiboot kernels and modules).
run: $(KERNEL) $(INITRD) $(DISK)
	$(QEMU) -kernel $(KERNEL) -initrd $(INITRD) $(QEMUFLAGS)

# Bootable CD image (needs grub-mkrescue + xorriso).
iso: $(ISO)
$(ISO): $(KERNEL) $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/vortex.bin
	cp $(INITRD) $(BUILD)/iso/boot/initrd.tar
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso

run-iso: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMUFLAGS)

# Smoke test without QEMU: boots the kernel in the Unicorn CPU emulator
# (pip install unicorn), drives the shell and the desktop, and writes
# screenshots to build/*.png.
test: $(KERNEL) $(INITRD)
	python3 tools/emu_test.py $(KERNEL) $(INITRD)

clean:
	rm -rf $(BUILD)

.SECONDARY:
-include $(OBJS:.o=.d) $(wildcard $(BUILD)/apps/*/*.d) $(wildcard $(BUILD)/sdk/*.d)
