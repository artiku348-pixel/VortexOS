#!/usr/bin/env python3
"""Smoke test: boot VortexOS inside the Unicorn CPU emulator (no QEMU needed).

Unicorn emulates only the CPU, so this script plays the rest of the PC:
the Multiboot bootloader (kernel + initrd module), the interrupt controller,
the PS/2 keyboard and mouse, the CMOS clock, the serial port and a Bochs VBE
graphics card with a linear framebuffer. Whenever the kernel idles in `hlt`
we deliver the next timer / keyboard / mouse interrupt through the kernel's
own IDT. Screenshots of the framebuffer are written to build/*.png.

The kernel is 64-bit. Its 32-bit boot code (boot/boot.S) runs first in a
separate 32-bit CPU until it has switched to long mode; the memory it has
prepared (page tables, GDT) is then handed to the 64-bit CPU that runs the
rest, because Unicorn cannot change the register width of a running CPU.
"""
import os
import struct
import threading
import sys
import zlib

from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_MODE_64, UC_HOOK_INSN, UC_HOOK_INTR, UC_HOOK_CODE,
                     UC_ERR_INSN_INVALID)
from unicorn.unicorn_py3.unicorn import UcContext
from unicorn.x86_const import *

RAM = 128 * 1024 * 1024
MBI = 0x9000
LFB = 0xFD000000
UNTIL = 0x100000                # emu_start() end address: mapped, never executed
UNTIL_USER = 0xFFFFFFF000      # same, for ring 3: top page of the user stack
EFER = 0xC0000080
HANG_SECONDS = 120              # a single run() may not take longer
SLICE = 20000                   # microseconds of emulation per time slice
ERROR_CODE = (8, 10, 11, 12, 13, 14, 17)
W, H = 1024, 768

LOWER = "\x00\x001234567890-=\x00\x00qwertyuiop[]\n\x00asdfghjkl;'`\x00\\zxcvbnm,./"
UPPER = "\x00\x00!@#$%^&*()_+\x00\x00QWERTYUIOP{}\n\x00ASDFGHJKL:\"~\x00|ZXCVBNM<>?"
PLAIN = {c: i for i, c in enumerate(LOWER) if c != "\x00"}
SHIFT = {c: i for i, c in enumerate(UPPER) if c != "\x00" and c != "\n"}
PLAIN[" "] = 0x39
EXTENDED = {"UP": 0x48, "DOWN": 0x50, "LEFT": 0x4B, "RIGHT": 0x4D, "DELETE": 0x53}


def load_elf(uc, data):
    """Load the kernel (64-bit code in an ELF32 container). Returns the entry
    point, the end of the image and its symbol table."""
    assert data[:4] == b"\x7fELF" and data[4] == 1, "expected an ELF32 container"
    assert struct.pack("<I", 0x1BADB002) in data[:0x3000], "no multiboot header"
    entry, phoff, shoff = struct.unpack_from("<III", data, 24)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 42)
    end = 0
    for i in range(phnum):
        p_type, off, _, paddr, filesz, memsz, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1:
            uc.mem_write(paddr, data[off:off + filesz])
            end = max(end, paddr + memsz)
    symbols = {}
    for i in range(shnum):
        _, sh_type, _, _, off, size, link, _, _, entsize = struct.unpack_from("<10I", data, shoff + i * shentsize)
        if sh_type != 2:                # SHT_SYMTAB
            continue
        stroff = struct.unpack_from("<I", data, shoff + link * shentsize + 16)[0]
        for pos in range(off, off + size, entsize):
            name, value = struct.unpack_from("<II", data, pos)
            symbols[data[stroff + name:data.index(b"\0", stroff + name)].decode()] = value
    return entry, end, symbols


def boot_stub(kernel, entry, end, symbols, magic, mbi):
    """Run the 32-bit part of boot.S like a Multiboot loader would start it.

    Returns the kernel memory it has prepared and the control registers at
    the moment the CPU arrives at the first 64-bit instruction."""
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    size = (end - 0x100000 + 0xFFF) & ~0xFFF
    uc.mem_map(0, 0x100000 + size)
    load_elf(uc, kernel)
    uc.reg_write(UC_X86_REG_EAX, magic)
    uc.reg_write(UC_X86_REG_EBX, mbi)
    uc.reg_write(UC_X86_REG_CR0, 0x11)          # protected mode, paging off
    target = symbols["long_mode_entry"]
    uc.emu_start(entry, target, count=100000)
    state = {"cr0": uc.reg_read(UC_X86_REG_CR0), "cr3": uc.reg_read(UC_X86_REG_CR3),
             "cr4": uc.reg_read(UC_X86_REG_CR4), "efer": uc.msr_read(EFER),
             "gdtr": uc.reg_read(UC_X86_REG_GDTR), "cs": uc.reg_read(UC_X86_REG_CS),
             "args": (uc.reg_read(UC_X86_REG_EDI), uc.reg_read(UC_X86_REG_ESI))}
    assert state["cs"] == 0x08 and state["cr0"] & 0x80000001 == 0x80000001 and state["cr4"] & 0x20 \
        and state["efer"] & 0x500 == 0x500, "boot code did not enter long mode: %r" % state
    assert state["args"] == (magic, mbi), "boot code lost the Multiboot arguments"
    return bytes(uc.mem_read(0x100000, size)), state


class Machine:
    def __init__(self, kernel, initrd=None, disk=None):
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_64)
        uc.mem_map(0, RAM)
        uc.mem_map(LFB, 4 * 1024 * 1024)
        self.serial = bytearray()
        self.dlab = False
        self.ps2 = []                   # bytes waiting in the 8042 output buffer
        self.to_mouse = False
        self.masks = {0x21: 0xFF, 0xA1: 0xFF}
        self.bga_index, self.bga = 0, {0: 0xB0C5}
        self.pci_addr = 0
        self.cmos_reg = 0
        self.pending = None
        self.hung = False
        self.disk = disk                # bytearray with the ATA disk contents, or None
        self.ata = {"regs": [0] * 8, "buf": bytearray(), "status": 0x50, "write": None}
        self.disk_writes = 0
        self.seg_off = None
        self.fault_off = None
        self.fresh = bytes(uc.context_save())
        self.to_user = False            # the next instruction is an iret to ring 3
        self.user_entries = 0           # kernel entries from ring 3
        self.mouse = [W // 2, H // 2]   # where the kernel believes the pointer is
        self.buttons = 0

        image = open(kernel, "rb").read()
        entry, kernel_end, symbols = load_elf(uc, image)
        # 32-bit boot code: builds the page tables and enters long mode
        memory, boot = boot_stub(image, entry, kernel_end, symbols, 0x2BADB002, MBI)
        uc.mem_write(0x100000, memory)
        uc.reg_write(UC_X86_REG_GDTR, boot["gdtr"])
        uc.reg_write(UC_X86_REG_CR3, boot["cr3"])
        uc.reg_write(UC_X86_REG_CR4, boot["cr4"])
        uc.msr_write(EFER, boot["efer"])
        uc.reg_write(UC_X86_REG_CR0, boot["cr0"])
        self.pc = symbols["long_mode_entry"]
        # the two places where the kernel returns to ring 3 (boot.S, isr.S):
        # `xor %ebp, %ebp; iretq` and `add $16, %rsp; iretq`
        for pattern in (b"\x31\xed\x48\xcf", b"\x48\x83\xc4\x10\x48\xcf"):
            image = bytes(uc.mem_read(0x100000, kernel_end - 0x100000))
            assert image.count(pattern) == 1, "cannot find the kernel's iretq instructions"
            addr = 0x100000 + image.index(pattern) + len(pattern) - 2
            uc.hook_add(UC_HOOK_CODE, self.watch_iret, None, addr, addr)
        flags, mods = 1, b""
        if initrd:
            blob = open(initrd, "rb").read()
            start = (kernel_end + 0xFFF) & ~0xFFF
            uc.mem_write(start, blob)
            uc.mem_write(MBI + 0x100, struct.pack("<IIII", start, start + len(blob), 0, 0))
            flags |= 8
            mods = struct.pack("<II", 1, MBI + 0x100)
        uc.mem_write(MBI, struct.pack("<IIIII", flags, 640, RAM // 1024 - 1024, 0, 0) + mods)
        uc.reg_write(UC_X86_REG_RDI, boot["args"][0])
        uc.reg_write(UC_X86_REG_RSI, boot["args"][1])
        uc.hook_add(UC_HOOK_INSN, self.port_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, self.port_out, None, 1, 0, UC_X86_INS_OUT)
        uc.hook_add(UC_HOOK_INTR, self.soft_int)

    # ---- devices ---------------------------------------------------------
    # ---- ATA disk (primary master, PIO, LBA28) ---------------------------
    def ata_lba(self):
        r = self.ata["regs"]
        return r[3] | r[4] << 8 | r[5] << 16 | (r[6] & 0x0F) << 24

    def ata_command(self, cmd):
        a = self.ata
        a["status"] = 0x50              # ready
        if cmd == 0xEC:                 # IDENTIFY DEVICE
            ident = [0] * 256
            ident[49] = 0x0200          # LBA supported
            ident[60], ident[61] = (len(self.disk) // 512) & 0xFFFF, (len(self.disk) // 512) >> 16
            a["buf"] = bytearray(struct.pack("<256H", *ident))
            a["status"] = 0x58          # ready + data request
        elif cmd in (0x20, 0x30):
            lba = self.ata_lba()
            assert a["regs"][2] == 1 and a["regs"][6] & 0x40, "driver must use LBA, 1 sector"
            if lba * 512 >= len(self.disk):
                a["status"] = 0x51      # error
            elif cmd == 0x20:
                a["buf"] = bytearray(self.disk[lba * 512:lba * 512 + 512])
                a["status"] = 0x58
            else:
                a["buf"], a["write"] = bytearray(), lba
                a["status"] = 0x58
        elif cmd != 0xE7:               # FLUSH CACHE needs nothing
            a["status"] = 0x51

    def ata_in(self, port, size):
        a = self.ata
        if port == 0x1F0:
            assert size == 2 and a["status"] & 8 and a["write"] is None, "unexpected ATA data read"
            value = a["buf"][0] | a["buf"][1] << 8
            del a["buf"][:2]
            if not a["buf"]:
                a["status"] = 0x50
            return value
        return a["status"] if port in (0x1F7, 0x3F6) else a["regs"][port - 0x1F0]

    def ata_out(self, port, size, value):
        a = self.ata
        if port == 0x1F0:
            assert size == 2 and a["write"] is not None, "unexpected ATA data write"
            a["buf"] += struct.pack("<H", value & 0xFFFF)
            if len(a["buf"]) == 512:
                self.disk[a["write"] * 512:a["write"] * 512 + 512] = a["buf"]
                a["buf"], a["write"], a["status"] = bytearray(), None, 0x50
                self.disk_writes += 1
        elif port == 0x1F7:
            self.ata_command(value & 0xFF)
        elif port != 0x3F6:
            a["regs"][port - 0x1F0] = value & 0xFF

    def port_in(self, uc, port, size, _):
        if self.disk is not None and (0x1F0 <= port <= 0x1F7 or port == 0x3F6):
            return self.ata_in(port, size)
        if port == 0x60:
            return self.ps2.pop(0) if self.ps2 else 0
        if port == 0x64:
            return 1 if self.ps2 else 0
        if port == 0x3FD:
            return 0x20                 # UART transmitter empty
        if port == 0x1CF:
            return self.bga.get(self.bga_index, 0)
        if port == 0xCFC:
            bus, dev, fn, off = (self.pci_addr >> 16) & 0xFF, (self.pci_addr >> 11) & 0x1F, \
                                (self.pci_addr >> 8) & 7, self.pci_addr & 0xFC
            if (bus, dev, fn) != (0, 2, 0):
                return 0xFFFFFFFF
            return {0x00: 0x11111234, 0x08: 0x03000002, 0x10: LFB | 8}.get(off, 0)
        if port == 0x71:                # CMOS clock: 2026-11-13 12:34:56, BCD, 24h
            return {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x07: 0x13, 0x08: 0x11,
                    0x09: 0x26, 0x0B: 0x02}.get(self.cmos_reg, 0)
        return self.masks.get(port, 0)

    def port_out(self, uc, port, size, value, _):
        if self.disk is not None and (0x1F0 <= port <= 0x1F7 or port == 0x3F6):
            return self.ata_out(port, size, value)
        if port == 0x3FB:
            self.dlab = bool(value & 0x80)
        elif port == 0x3F8 and not self.dlab:
            self.serial.append(value & 0xFF)
        elif port in self.masks:
            self.masks[port] = value & 0xFF
        elif port == 0x64:
            if value == 0x20:
                self.ps2.append(0x45)   # controller configuration byte
            self.to_mouse = value == 0xD4
        elif port == 0x60 and self.to_mouse:
            self.to_mouse = False
            self.ps2.append(0xFA)       # mouse ACK
        elif port == 0x1CE:
            self.bga_index = value
        elif port == 0x1CF:
            self.bga[self.bga_index] = value
        elif port == 0xCF8:
            self.pci_addr = value
        elif port == 0x70:
            self.cmos_reg = value & 0x7F

    # ---- CPU -------------------------------------------------------------
    def to_ring0(self, cs_sel, ss_sel):
        """Switch the emulated CPU to ring 0 (what an interrupt from ring 3 does).

        Unicorn refuses privilege changes through reg_write, so we edit a saved
        CPU context: the segment caches of CS/SS and the CPL bits in hflags."""
        uc = self.uc
        ctx = uc.context_save()
        blob = bytearray(bytes(ctx))
        if self.seg_off is None:        # locate CPUX86State.segs[] by its contents
            sels = [uc.reg_read(r) for r in (UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS,
                                             UC_X86_REG_DS, UC_X86_REG_FS, UC_X86_REG_GS)]
            hits = [off for off in range(8, len(blob) - 144, 4)
                    if all(struct.unpack_from("<I", blob, off + i * 24)[0] == sel
                           for i, sel in enumerate(sels))
                    and struct.unpack_from("<I", blob, off + 24 + 16)[0] == 0xFFFFFFFF
                    and struct.unpack_from("<I", blob, off - 8)[0] & 3 == 3]
            assert len(hits) == 1, "cannot locate segment state in the Unicorn context"
            self.seg_off = hits[0]
        _, gdt, _, _ = uc.reg_read(UC_X86_REG_GDTR)
        for index, sel in ((1, cs_sel), (2, ss_sel)):
            desc = struct.unpack("<II", uc.mem_read(gdt + (sel & ~7), 8))[1]
            struct.pack_into("<IIQII", blob, self.seg_off + index * 24, sel, 0, 0, 0xFFFFFFFF,
                             (desc & 0x00F0FF00) | 0x100)
        hflags = struct.unpack_from("<I", blob, self.seg_off - 8)[0]
        struct.pack_into("<I", blob, self.seg_off - 8, hflags & ~3)

        self.patch_context(blob, ctx)

    def patch_context(self, blob, ctx):
        patched = UcContext.__new__(UcContext)
        patched.__setstate__((bytes(blob), ctx.size, ctx.arch, ctx.mode))
        self.uc.context_restore(patched)

    def clear_fault(self, vector):
        """Forget the exception Unicorn has just reported to us.

        The emulator remembers it until it is delivered (to detect double
        faults), but delivery is done by this script, so the next page fault
        would be reported as a double fault. Reset CPUX86State.old_exception."""
        ctx = self.uc.context_save()
        blob = bytearray(bytes(ctx))
        if self.fault_off is None:
            hits = [off for off in range(0, len(blob) - 3, 4)
                    if struct.unpack_from("<I", blob, off)[0] == vector
                    and self.fresh[off:off + 4] == b"\xff\xff\xff\xff"]
            if len(hits) != 1:
                return                  # not recorded for this kind of exception
            self.fault_off = hits[0]
        struct.pack_into("<i", blob, self.fault_off, -1)
        self.patch_context(blob, ctx)

    def enter_gate(self, vector, return_pc):
        """Deliver an interrupt/exception like a CPU in long mode; returns the handler."""
        uc = self.uc
        _, base, _, _ = uc.reg_read(UC_X86_REG_IDTR)
        lo, sel, ist, typ, mid, hi, _ = struct.unpack("<HHBBHII", uc.mem_read(base + vector * 16, 16))
        assert typ & 0x8E == 0x8E and sel == 0x08 and ist == 0, "bad IDT gate %d" % vector
        flags = uc.reg_read(UC_X86_REG_RFLAGS)
        cs = uc.reg_read(UC_X86_REG_CS)
        rsp = uc.reg_read(UC_X86_REG_RSP)
        frame = [uc.reg_read(UC_X86_REG_SS), rsp, flags, cs, return_pc]
        if cs & 3:                      # from ring 3: switch to the kernel stack in the TSS
            assert vector < 48 or typ & 0x60 == 0x60, "int %#x gate is not callable from ring 3" % vector
            _, gdt, _, _ = uc.reg_read(UC_X86_REG_GDTR)
            d = uc.mem_read(gdt + 0x28, 16)
            tss = d[2] | d[3] << 8 | d[4] << 16 | d[7] << 24 | struct.unpack_from("<I", d, 8)[0] << 32
            rsp = struct.unpack("<Q", uc.mem_read(tss + 4, 8))[0]
            self.to_ring0(sel, 0x10)
            self.user_entries += 1
        rsp &= ~0xF                     # long mode aligns the stack
        if vector in ERROR_CODE:
            frame.append(4 if cs & 3 else 0)
        rsp -= 8 * len(frame)
        uc.mem_write(rsp, struct.pack("<%dQ" % len(frame), *reversed(frame)))
        uc.reg_write(UC_X86_REG_RSP, rsp)
        if typ & 0x0F == 0x0E:          # interrupt gates clear IF, trap gates do not
            uc.reg_write(UC_X86_REG_RFLAGS, flags & ~0x200)
        return lo | mid << 16 | hi << 32

    def watch_iret(self, uc, address, size, _):
        if self.to_user:                # second visit: let it execute
            self.to_user = False
        elif struct.unpack("<Q", uc.mem_read(uc.reg_read(UC_X86_REG_RSP) + 8, 8))[0] & 3:
            self.to_user = True         # restart the run with the user end address
            uc.emu_stop()

    def soft_int(self, uc, intno, _):
        # Unicorn reports `int N` and CPU exceptions to the host instead of
        # using the guest IDT: stop, then deliver them ourselves in run().
        self.pending = intno
        self.cr2 = uc.reg_read(UC_X86_REG_CR2)   # the stop below may clobber it
        uc.emu_stop()

    def run(self):
        """Run until the CPU idles in `hlt` (returns True if interrupts are on).

        If a user program keeps the CPU busy, play the timer: preempt it at the
        end of each time slice and return None after a few slices."""
        watchdog = threading.Timer(HANG_SECONDS, self.hang)
        watchdog.daemon = True
        watchdog.start()
        try:
            return self.run_slices()
        finally:
            watchdog.cancel()

    def hang(self):
        self.hung = True
        self.uc.emu_stop()

    def run_slices(self):
        busy = 0
        for _ in range(100000):
            self.pending = None
            try:
                # Unicorn touches the end address when it stops, so it must be
                # mapped at the privilege level the CPU stops in; a stray fault
                # there would turn the next real fault into a double fault.
                # watch_iret() makes sure a run never crosses from ring 0 to 3.
                #
                # Kernel code runs until it halts, with no time limit: a stop
                # in the middle of an I/O instruction would make Unicorn repeat
                # it (and e.g. read a word from the disk twice). Only user
                # code gets time slices; a watchdog catches a stuck kernel.
                user = self.to_user or self.uc.reg_read(UC_X86_REG_CS) & 3
                self.uc.emu_start(self.pc, UNTIL_USER if user else UNTIL,
                                  timeout=SLICE if user else 0)
            except UcError as e:
                if e.errno != UC_ERR_INSN_INVALID:
                    raise
                self.pending = 6        # #UD
            eip = self.uc.reg_read(UC_X86_REG_RIP)
            assert not self.hung, "kernel hang at %#x" % eip
            if self.pending is not None:
                if self.pending < 32:
                    self.clear_fault(self.pending)
                if self.pending == 14:
                    self.uc.reg_write(UC_X86_REG_CR2, self.cr2)
                self.pc = self.enter_gate(self.pending, eip)
                continue
            self.pc = eip
            ring3 = self.uc.reg_read(UC_X86_REG_CS) & 3
            interrupts = bool(self.uc.reg_read(UC_X86_REG_RFLAGS) & 0x200)
            if not ring3 and self.uc.mem_read(eip - 1, 1)[0] == 0xF4:
                return interrupts
            if ring3:                   # time slice used up in user mode
                busy += 1
                if busy > 3:
                    return None
                self.pc = self.enter_gate(32, self.pc)
        raise AssertionError("kernel hang at %#x" % self.pc)

    def irq(self, n):
        mask = self.masks[0x21] if n < 8 else self.masks[0xA1]
        assert not mask & (1 << (n & 7)), "IRQ%d is masked" % n
        assert self.uc.reg_read(UC_X86_REG_RFLAGS) & 0x200, "interrupts are disabled"
        self.pc = self.enter_gate(32 + n, self.pc)
        return self.run()

    # ---- input helpers ---------------------------------------------------
    def tick(self, n=1):
        for _ in range(n):
            self.irq(0)

    def scancodes(self, *codes):
        alive = True
        for code in codes:
            self.ps2.append(code)
            alive = self.irq(1)
        return alive

    def type(self, text):
        for ch in text:
            if ch in PLAIN:
                self.scancodes(PLAIN[ch], PLAIN[ch] | 0x80)
            else:
                self.scancodes(0x2A, SHIFT[ch], SHIFT[ch] | 0x80, 0xAA)

    def key(self, name):
        self.scancodes(0xE0, EXTENDED[name], 0xE0, EXTENDED[name] | 0x80)

    def mouse_packet(self, dx, dy, buttons):
        self.buttons = buttons
        self.mouse[0] = min(max(self.mouse[0] + dx, 0), W - 1)
        self.mouse[1] = min(max(self.mouse[1] + dy, 0), H - 1)
        dy = -dy                        # PS/2 Y axis points up
        head = 0x08 | buttons | (0x10 if dx < 0 else 0) | (0x20 if dy < 0 else 0)
        for byte in (head, dx & 0xFF, dy & 0xFF):
            self.ps2.append(byte)
            self.irq(12)

    def move_to(self, x, y):
        while (self.mouse[0], self.mouse[1]) != (x, y):
            dx = max(-100, min(100, x - self.mouse[0]))
            dy = max(-100, min(100, y - self.mouse[1]))
            self.mouse_packet(dx, dy, self.buttons)

    def click(self, x, y):
        self.move_to(x, y)
        self.mouse_packet(0, 0, 1)
        self.mouse_packet(0, 0, 0)

    def drag(self, x0, y0, x1, y1):
        self.move_to(x0, y0)
        self.mouse_packet(0, 0, 1)
        self.move_to(x1, y1)
        self.mouse_packet(0, 0, 0)

    # ---- output helpers --------------------------------------------------
    def text_screen(self):
        raw = self.uc.mem_read(0xB8000, 80 * 25 * 2)
        rows = [bytes(raw[r * 160:(r + 1) * 160:2]).decode("cp437").rstrip() for r in range(25)]
        return "\n".join(rows).rstrip()

    def output(self):
        return self.serial.decode("ascii", "replace").replace("\r", "")

    def pixel(self, x, y):
        return struct.unpack("<I", self.uc.mem_read(LFB + (y * W + x) * 4, 4))[0] & 0xFFFFFF

    def screenshot(self, path):
        raw = self.uc.mem_read(LFB, W * H * 4)
        rgb = bytearray(W * H * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
        rows = b"".join(b"\x00" + bytes(rgb[y * W * 3:(y + 1) * W * 3]) for y in range(H))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) \
            + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        open(path, "wb").write(png)


def expect(m, *needles):
    out = m.output()
    for needle in needles:
        assert needle in out, "missing %r in output:\n%s" % (needle, out[-2000:])


def fat_tools():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkfat
    return mkfat, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "diskfs")


def settle(m):
    """Let the diskd thread write everything out."""
    m.tick(150)
    mark = len(m.output())
    m.type("disk\n")
    assert "everything is saved" in m.output()[mark:], m.output()[mark:]


def disk_test(kernel, initrd, bits, mbr=False, gui=False):
    """FAT volume: mount, read, change, verify with an independent reader, reboot."""
    mkfat, src = fat_tools()
    size = {12: 2, 16: 16, 32: 40}[bits]
    disk = bytearray(mkfat.Builder(size, bits).build(src, mbr=mbr))
    hello = open(os.path.join(os.path.dirname(kernel) or ".", "apps/bin/hello"), "rb").read()
    welcome = open(os.path.join(src, "welcome.txt"), "rb").read()
    free0 = mkfat.Volume(bytes(disk)).check()

    m = Machine(kernel, initrd, disk)
    assert m.run(), m.text_screen()
    expect(m, "Disk: FAT%d volume" % bits, "mounted at /disk")
    assert m.disk_writes == 0, "mounting must not write to the disk"
    for cmd in ("ls /disk", "cat /disk/welcome.txt", "ls /disk/Documents", "ps", "disk",
                "echo persistent data > /disk/saved.txt", "mkdir /disk/Projects",
                "echo long names work > /disk/Projects/MixedCase-LongFileName.text",
                "cp /bin/hello /disk/hello", "rm /disk/scripts/backup", "disk"):
        m.type(cmd + "\n")
    expect(m, "This file lives on the FAT disk", "Getting Started.txt", "kernel  diskd",
           "Volume: FAT%d" % bits, "everything is saved", "changes waiting to be written")
    if mbr:
        expect(m, "partition at sector %d" % mkfat.MBR_START)
    assert m.disk_writes == 0, "writes should be deferred"
    settle(m)                           # the diskd thread syncs after a quiet moment
    assert m.disk_writes > 0, "diskd did not write the changes"

    tree = mkfat.read_tree(bytes(disk))  # independent reader + consistency check
    assert tree["/saved.txt"] == b"persistent data\n", tree.get("/saved.txt")
    assert tree["/Projects/MixedCase-LongFileName.text"] == b"long names work\n"
    assert tree["/hello"] == hello and len(hello) > 4096, "multi-cluster file is damaged"
    assert tree["/Documents/Getting Started.txt"].startswith(b"Long file names")
    assert "/scripts/backup" not in tree and tree["/scripts"] is None
    assert tree["/welcome.txt"] == welcome

    # Only what changed is written: one small file must not rewrite the volume.
    writes = m.disk_writes
    m.type("echo second > /disk/saved.txt\nsync\n")
    assert 0 < m.disk_writes - writes <= 4, "sync wrote %d sectors" % (m.disk_writes - writes)
    assert mkfat.read_tree(bytes(disk))["/saved.txt"] == b"second\n"

    # Many long names: the directory has to grow; then free and reuse entries.
    names = ["long-file-name-number-%02d.txt" % i for i in range(40)]
    for name in names:
        m.type("touch /disk/Projects/%s\n" % name)
    for name in names[::2]:
        m.type("rm /disk/Projects/%s\n" % name)
    for cmd in ("echo reused > /disk/Projects/x.txt",
                "echo reused slots > /disk/Projects/A-Name-That-Needs-Three-Entries.txt",
                "echo more >> /disk/saved.txt",             # append
                "echo tiny > /disk/hello",                  # shrink: clusters are freed
                "cp /bin/hello /disk/Projects/hello.elf",
                "rm /disk/scripts",                         # remove an (empty) directory
                "mkdir /disk/Projects/sub", "mkdir /disk/Projects/sub/deeper",
                "echo deep > /disk/Projects/sub/deeper/file.txt",
                "cat /disk/SAVED.TXT",                      # names ignore case
                "echo same file >> /disk/Saved.Txt",
                "echo bad > /disk/what?.txt",               # not allowed on FAT
                "ls /disk/Projects"):
        m.type(cmd + "\n")
    settle(m)
    expect(m, "\nsecond\nmore\n")
    volume = mkfat.Volume(bytes(disk))
    free1 = volume.check()
    tree = volume.tree
    expected = {"/Documents": None, "/Projects": None, "/Projects/sub": None,
                "/Projects/sub/deeper": None,
                "/Documents/Getting Started.txt": tree["/Documents/Getting Started.txt"],
                "/welcome.txt": welcome, "/saved.txt": b"second\nmore\nsame file\n",
                "/hello": b"tiny\n", "/Projects/hello.elf": hello,
                "/Projects/MixedCase-LongFileName.text": b"long names work\n",
                "/Projects/x.txt": b"reused\n",
                "/Projects/A-Name-That-Needs-Three-Entries.txt": b"reused slots\n",
                "/Projects/sub/deeper/file.txt": b"deep\n"}
    expected.update({"/Projects/" + name: b"" for name in names[1::2]})
    assert tree == expected, sorted(set(tree) ^ set(expected))
    cluster = volume.spc * 512
    assert free0 - free1 < len(hello) + 24 * cluster, "space was not freed: %d" % (free0 - free1)
    try:                                # a third opinion, if pyfatfs is installed
        from pyfatfs.PyFatFS import PyFatFS
        import tempfile
        with tempfile.NamedTemporaryFile() as f:
            f.write(disk)
            f.flush()
            other = PyFatFS(f.name, read_only=True, offset=volume.base * 512)
            assert other.fs.fat_type == bits
            assert other.readbytes("/Projects/HELLO.ELF") == hello
            assert other.readbytes("/Projects/SUB/DEEPER/FILE.TXT") == b"deep\n"
            assert len(other.listdir("/Projects")) == 25
            other.close()
        third = ", pyfatfs agrees"
    except ImportError:
        third = ""
    print(m.text_screen())

    m = Machine(kernel, initrd, disk)   # "reboot": a new machine with the same disk
    assert m.run(), m.text_screen()
    mark = len(m.output())
    for cmd in ("cat /disk/saved.txt", "cat /disk/Projects/MixedCase-LongFileName.text",
                "ls /disk/Projects", "cat /disk/Projects/sub/deeper/file.txt",
                "/disk/Projects/hello.elf from disk", "kill 2", "ps"):
        m.type(cmd + "\n")
    out = m.output()[mark:]
    for needle in ("\nsecond\nmore\nsame file\n", "\nlong names work\n", "\ndeep\n",
                   "long-file-name-number-39.txt", "A-Name-That-Needs-Three-Entries.txt",
                   "Hello from a VortexOS application!", "Arguments: [from] [disk]",
                   "kernel  diskd"):
        assert needle in out, "missing %r in:\n%s" % (needle, out)
    assert "long-file-name-number-38.txt" not in out
    assert m.disk_writes == 0, "reading must not write"
    print(m.text_screen())

    if gui:     # File manager on the desktop: browse /disk, view a file, delete it.
        shots = os.path.dirname(kernel) or "."
        m.type("desktop\n")
        m.tick(2)
        m.type("files /disk &\n")
        m.tick(2)
        m.screenshot(shots + "/08-files.png")
        for _ in range(6):
            m.key("DOWN")               # the last entry: welcome.txt
        m.type("\n")                    # open it in the viewer
        m.tick(2)
        m.screenshot(shots + "/09-files-view.png")
        m.key("LEFT")                   # back to the list
        m.key("DELETE")
        m.tick(150)
        after = mkfat.read_tree(bytes(disk))
        assert "/welcome.txt" not in after and "/saved.txt" in after, sorted(after)
    print("\n--- FAT%d%s OK: %d entries verified by an independent reader%s ---\n"
          % (bits, " in an MBR partition" if mbr else "", len(tree), third))


def disk_full_test(kernel, initrd):
    """A tiny FAT12 volume: fill it up, then make room again."""
    mkfat, src = fat_tools()
    disk = bytearray(mkfat.Builder(bits=12, sectors=400).build(src))
    hello = open(os.path.join(os.path.dirname(kernel) or ".", "apps/bin/hello"), "rb").read()
    fit = mkfat.Volume(bytes(disk)).check() // ((len(hello) + 511) // 512 * 512)
    count = fit + 4
    m = Machine(kernel, initrd, disk)
    assert m.run(), m.text_screen()
    for i in range(count):
        m.type("cp /bin/hello /disk/h%02d\n" % i)
    m.tick(150)
    m.type("disk\n")
    expect(m, "no space left on /disk", "DISK FULL")
    volume = mkfat.Volume(bytes(disk))  # still a consistent volume
    volume.check()
    saved = [p for p, data in volume.tree.items() if data == hello]
    assert len(saved) == fit, "%d files saved, %d fit" % (len(saved), fit)

    for i in range(8):                  # free some space: the rest gets written
        m.type("rm /disk/h%02d\n" % i)
    settle(m)
    volume = mkfat.Volume(bytes(disk))
    volume.check()
    assert sorted(p for p, data in volume.tree.items() if data == hello) == \
        ["/h%02d" % i for i in range(8, count)]
    print(m.text_screen())
    print("\n--- disk full OK (%d files fit, %d copied) ---\n" % (fit, count))


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/vortex.bin"
    initrd = sys.argv[2] if len(sys.argv) > 2 else "build/initrd.tar"
    shots = os.path.dirname(kernel) or "."
    m = Machine(kernel, initrd)

    # ---- text mode: boot, shell, PATH, scripts, programs -----------------
    assert m.run(), "kernel halted with interrupts off:\n" + m.text_screen()
    assert m.text_screen().endswith("vortex:/home>"), m.text_screen()
    expect(m, "[ OK ] Root filesystem", "Graphics adapter (Bochs VBE)", "Welcome to VortexOS!")

    m.tick(250)
    for cmd in ("help", "ls /bin", "which hello", "which greet", "which ls", "env",
                "greet World", "cat /tmp/greet.log", "hello one two", "hello",
                "echo saved text > note.txt", "echo more >> note.txt", "cat note.txt",
                "hexdump note.txt", "mkproject demo", "cd demo", "pwd", "cat README.txt",
                "cd ..", "cp note.txt demo", "mv demo/note.txt demo/n2", "ls demo", "rm demo/n2",
                "uptime", "date", "ps", "about", "hello > out.txt", "cat out.txt", "nosuchcmd"):
        m.type(cmd + "\n")
    expect(m,
           "/bin/hello (program)", "/bin/greet (script)", "ls: shell built-in",
           "PATH=/bin:/apps", "USER=user",
           "Hello, World! I am script greet and your PATH is /bin:/apps",
           "logged a greeting for World",
           "Arguments: [one] [two]", "Nice to meet you, user.", "Tip: ",
           "saved text\nmore\n", "00000000  73 61 76 65 64",
           "/home/demo\n", "# demo\ncreated by user\n", "      16  n2",
           "up 00:00:02", "2026-11-13 12:34:56", "   1  kernel  idle",
           "about: needs the desktop",
           "nosuchcmd: command not found")
    assert m.output().count("Hello from a VortexOS application!") == 3

    m.type("guess\n50\n\n")             # interactive program reading the keyboard
    expect(m, "Your guess: 50", "It was ")
    m.type("guess\n")
    m.scancodes(0x1D, 0x2E, 0xAE, 0x9D) # Ctrl+C kills the foreground program
    assert m.output().endswith("^C\nvortex:/home> "), m.output()[-200:]
    m.type("ps\n")
    assert "guess" not in m.output().split("^C")[-1]
    print(m.text_screen())
    print("\n--- text mode OK ---\n")

    # ---- user mode: isolation, paging, preemption --------------------------
    import re
    def free_frames():
        m.type("mem\n")
        return int(re.findall(r"(\d+) free \(", m.output())[-1])
    m.type("clear\n")
    frames = free_frames()
    assert m.user_entries > 50, "programs did not run in ring 3"
    for mode in ("malloc", "badptr", "null", "kernel", "priv"):
        m.type("systest %s\n" % mode)
    expect(m, "malloc: 64 blocks, checksum 2016, reuse ok",
           "bad pointers returned -1 -1 -1",
           "systest crashed: Page fault (RIP=00000080", "address 0000000000000000)",
           "address 0000000000100000)",
           "systest crashed: General protection fault")
    assert "broken" not in m.output()
    assert free_frames() == frames, "page frames leaked"

    m.type("systest spin &\n")          # a busy loop must not freeze the system
    spin = int(re.findall(r"\[(\d+)\] systest", m.output())[-1])
    before = m.user_entries
    m.type("ps\necho still responsive\n")
    expect(m, "%4d  user    systest" % spin, "\nstill responsive\n")
    assert m.user_entries > before + 10, "the spinning program was not preempted"
    m.type("kill %d\n" % spin)
    assert m.tick() is None or True
    m.type("ps\n")
    assert "systest" not in m.output().split("kill %d" % spin)[-1]
    assert free_frames() == frames, "page frames leaked"
    print(m.text_screen())
    print("\n--- user mode OK (%d kernel entries from ring 3) ---\n" % m.user_entries)

    # ---- desktop ----------------------------------------------------------
    m.type("desktop\n")
    m.tick(2)
    assert m.bga.get(1) == W and m.bga.get(2) == H and m.bga.get(4) == 0x41, m.bga
    m.screenshot(shots + "/01-desktop.png")
    mark = len(m.output())
    m.type("hello\nls /apps\ncalc &\n")
    m.tick(2)
    out = m.output()[mark:]
    assert "Hello from a VortexOS application!" in out and "snake" in out, out

    # Calculator (window 2, started from the terminal): 7 * 6 = with the mouse.
    def calc_button(col, row):
        m.click(96 + 6 + col * 62 + 28, 104 + 64 + row * 62 + 28)
    for col, row in ((0, 0), (3, 1), (2, 1), (2, 3)):
        calc_button(col, row)
    m.screenshot(shots + "/02-terminal-calc.png")

    # Launcher menu -> Snake (window 3); steer with the arrow keys.
    m.click(40, 752)
    m.move_to(60, 693)
    m.screenshot(shots + "/03-menu.png")
    m.click(60, 693)
    m.tick(40)
    m.key("DOWN")
    m.tick(40)
    m.key("LEFT")
    m.tick(30)

    # Launcher menu -> Paint (window 4): pick red, draw, then drag the window.
    m.click(40, 752)
    m.click(60, 667)
    m.tick(2)
    m.drag(168 + 60, 164 + 80, 168 + 300, 164 + 260)
    m.click(168 + 44, 164 + 12)
    m.drag(168 + 300, 164 + 80, 168 + 60, 164 + 260)
    m.drag(168 + 200, 164 - 12, 700, 120)
    m.screenshot(shots + "/04-apps.png")
    m.type("ps\n")                      # keys go to the focused window (Paint), not the shell
    assert not m.output().endswith("ps\nvortex:/home> ")

    # Close Paint with its close box, focus the terminal, crash its shell.
    m.click(500 + 480 - 13, 132 - 12)
    m.click(80, 400)
    m.type("ps\n")
    tail = m.output()[mark:]
    assert "snake" in tail.split("ps\n")[-1] and "paint" not in tail.split("ps\n")[-1], tail[-300:]
    m.type("crash\n")
    expect(m, "shell crashed: Invalid opcode")
    m.tick(2)
    m.screenshot(shots + "/05-after-close.png")

    # Exit the desktop: back to a full-screen console on the framebuffer.
    m.click(40, 752)
    m.click(60, 719)
    m.tick(2)
    m.type("ps\necho back in the console\n")
    tail = m.output()[-200:]
    assert "back in the console" in tail and "snake" not in tail, tail
    m.type("desktop\n")                 # and it can be started again
    m.mouse = [W // 2, H // 2]
    m.tick(2)
    m.type("echo second run\n")
    m.click(40, 752)
    m.click(60, 719)
    m.type("mem\nps\n")
    assert "shell" not in m.output().split("ps\n")[-1], m.output()[-300:]
    m.screenshot(shots + "/06-console.png")

    # An exception in the kernel task itself halts the machine.
    m.type("crash")
    assert not m.scancodes(0x1C), "exception handler should halt with interrupts off"
    expect(m, "CPU EXCEPTION 6: Invalid opcode")
    m.screenshot(shots + "/07-panic.png")
    print(m.output()[mark:])
    print("--- desktop OK, screenshots in %s ---" % shots)
    disk_test(kernel, initrd, 16, gui=True)
    disk_test(kernel, initrd, 12)
    disk_test(kernel, initrd, 32, mbr=True)
    disk_full_test(kernel, initrd)
    print("\nPASS")


if __name__ == "__main__":
    main()
