# VortexOS build. Works on Linux, macOS and Windows (cmd.exe, PowerShell or
# MSYS): besides make it needs only the compiler and Python, all file
# operations go through tools/build.py.
#
# VortexOS is a 64-bit (x86-64) system.
#   Linux:    the host gcc on an x86-64 machine, in freestanding mode
#   Windows:  the x86_64-elf-gcc cross compiler (MinGW gcc cannot produce ELF)
#   macOS:    make CROSS=x86_64-elf-
ifeq ($(OS),Windows_NT)
CROSS   ?= x86_64-elf-
PYTHON  ?= python
else
CROSS   ?=
PYTHON  ?= python3
endif
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AR      := $(CROSS)ar
OBJCOPY := $(CROSS)objcopy
BUILD   := build
KERNEL  := $(BUILD)/vortex.bin
KERNEL64 := $(BUILD)/vortex.elf64
INITRD  := $(BUILD)/initrd.tar
ISO     := $(BUILD)/vortex.iso
QEMU    ?= qemu-system-x86_64
DISK    := $(BUILD)/disk.img
QEMUFLAGS ?= -m 128 -vga std -serial stdio -drive file=$(DISK),format=raw,if=ide,index=0
HELPER  := $(PYTHON) tools/build.py

# ---- kernel ---------------------------------------------------------------
# No SSE/x87 in the kernel (-mgeneral-regs-only) and no red zone: interrupts
# arrive on the stack of the code they interrupt.
CFLAGS  := -m64 -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
           -fcf-protection=none -mcmodel=small -mgeneral-regs-only -mno-red-zone \
           -Iinclude -Isdk/include -MMD -MP
ASFLAGS := -m64 -g
LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -z noexecstack -z max-page-size=0x1000

SRCS := $(wildcard boot/*.S) $(wildcard kernel/*.c)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

# ---- applications (SDK) ---------------------------------------------------
# Position-independent, freestanding ELF64 programs linked against libvx.
# apps/bin/NAME.c -> /bin/NAME (console), apps/gui/NAME.c -> /apps/NAME (GUI).
APP_CFLAGS  := -m64 -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
               -fno-stack-protector -fPIE -fvisibility=hidden \
               -fno-asynchronous-unwind-tables -fcf-protection=none \
               -mgeneral-regs-only -Isdk/include -MMD -MP
APP_LDFLAGS := -m elf_x86_64 -pie --no-dynamic-linker -nostdlib -e _start \
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
x86_64-elf-tools-windows.zip from github.com/lordmilko/i686-elf-tools/releases, \
unpack it and add its bin folder to PATH. See README.md)
endif
endif
endif

apps: $(APP_ELFS)

# The kernel is linked as 64-bit ELF (keep it for debuggers) and then wrapped
# into an ELF32 container: Multiboot 1 loaders (qemu -kernel, GRUB's
# `multiboot` command) only load 32-bit ELF files. The code inside is the same.
$(KERNEL64): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL): $(KERNEL64)
	$(OBJCOPY) -O elf32-i386 $< $@

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
