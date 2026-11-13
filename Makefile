# VortexOS build. Works on Linux, macOS and Windows (cmd.exe, PowerShell or
# MSYS): besides make it needs only the compiler and Python, all file
# operations go through tools/build.py.
#
#   Linux:    the host gcc in 32-bit freestanding mode (package gcc-multilib)
#   Windows:  the i686-elf-gcc cross compiler (MinGW gcc cannot produce ELF)
#   macOS:    make CROSS=i686-elf-
ifeq ($(OS),Windows_NT)
CROSS   ?= i686-elf-
PYTHON  ?= python
else
CROSS   ?=
PYTHON  ?= python3
endif
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
HELPER  := $(PYTHON) tools/build.py

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
ROOTFS_FILES := $(wildcard rootfs/* rootfs/*/* rootfs/*/*/* rootfs/*/*/*/*)

.PHONY: all apps run iso run-iso test clean FORCE disk
all: $(KERNEL) $(INITRD) $(DISK)
FORCE:

# ---- environment checks ---------------------------------------------------
# Output directories are created up front (no `mkdir -p` on Windows); the
# same call tells us whether Python works at all.
OUT_DIRS := $(sort $(BUILD) $(dir $(OBJS) $(APP_ELFS) $(LIBVX)))
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(shell $(HELPER) mkdir $(OUT_DIRS)),ok)
$(error Python 3 is required: '$(PYTHON)' did not run. Install it from python.org \
(tick "Add python.exe to PATH") or pass PYTHON=<path to python>)
endif
ifeq ($(OS),Windows_NT)
ifeq ($(findstring elf,$(shell $(HELPER) cc $(CC))),)
$(error '$(CC)' is missing or is not an ELF cross compiler. Download \
i686-elf-tools-windows.zip from github.com/lordmilko/i686-elf-tools/releases, \
unpack it and add its bin folder to PATH. See README.md)
endif
endif
endif

apps: $(APP_ELFS)

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.c.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/sdk/%.o: sdk/lib/%.c
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(LIBVX): $(BUILD)/sdk/libvx.o
	$(AR) rcs $@ $^

$(BUILD)/apps/%.o: apps/%.c
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/apps/%: $(BUILD)/apps/%.o $(LIBVX)
	$(LD) $(APP_LDFLAGS) -o $@ $< $(LIBVX)

# The initrd is a ustar archive of rootfs/ plus the compiled applications;
# the kernel unpacks it into the in-memory filesystem at boot.
empty :=
space := $(empty) $(empty)
comma := ,
$(INITRD): $(APP_ELFS) $(ROOTFS_FILES) FORCE
	$(HELPER) initrd $@ rootfs \
	    bin=$(subst $(space),$(comma),$(patsubst %,$(BUILD)/apps/bin/%,$(BIN_APPS))) \
	    apps=$(subst $(space),$(comma),$(patsubst %,$(BUILD)/apps/gui/%,$(GUI_APPS)))

# The hard disk: a 16 MiB FAT16 image seeded from diskfs/ and mounted at
# /disk. It is created only once, so files saved by the OS are kept between
# runs; `make disk` starts over with a fresh image.
$(DISK):
	$(PYTHON) tools/mkfat.py create $@ diskfs 16

disk:
	$(PYTHON) tools/mkfat.py create $(DISK) diskfs 16

# Boot directly in QEMU (it understands Multiboot kernels and modules).
run: $(KERNEL) $(INITRD) $(DISK)
	$(HELPER) qemu $(QEMU) -kernel $(KERNEL) -initrd $(INITRD) $(QEMUFLAGS)

# Bootable CD image (needs grub-mkrescue + xorriso: Linux only).
iso: $(ISO)
$(ISO): $(KERNEL) $(INITRD) boot/grub.cfg
	$(HELPER) copy $(KERNEL) $(BUILD)/iso/boot/vortex.bin
	$(HELPER) copy $(INITRD) $(BUILD)/iso/boot/initrd.tar
	$(HELPER) copy boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso

run-iso: $(ISO) $(DISK)
	$(HELPER) qemu $(QEMU) -cdrom $(ISO) $(QEMUFLAGS)

# Smoke test without QEMU: boots the kernel in the Unicorn CPU emulator
# (pip install unicorn), drives the shell and the desktop, and writes
# screenshots to build/*.png.
test: $(KERNEL) $(INITRD)
	$(PYTHON) tools/emu_test.py $(KERNEL) $(INITRD)

clean:
	$(HELPER) rmtree $(BUILD)

.SECONDARY:
-include $(OBJS:.o=.d) $(wildcard $(BUILD)/apps/*/*.d) $(wildcard $(BUILD)/sdk/*.d)
