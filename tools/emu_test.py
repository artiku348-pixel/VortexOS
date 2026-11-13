#!/usr/bin/env python3
"""Smoke test: boot the VortexOS kernel inside the Unicorn CPU emulator.

Unicorn emulates only the CPU, so this script plays the role of the
bootloader (Multiboot hand-off), the keyboard controller and the PIC:
every time the kernel idles in `hlt`, we deliver a timer or keyboard IRQ
through the kernel's own IDT. Console output is read from VGA memory/COM1.
"""
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INSN, UC_ERR_INSN_INVALID
from unicorn.x86_const import *

RAM = 32 * 1024 * 1024
MBI = 0x9000
SCANCODES = {c: i for i, c in enumerate(
    "\x00\x001234567890-=\x00\x00qwertyuiop[]\n\x00asdfghjkl;'`\x00\\zxcvbnm,./") if c != "\x00"}
SCANCODES[" "] = 0x39


def load_elf(uc, path):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 1, "expected a 32-bit ELF"
    assert struct.pack("<I", 0x1BADB002) in data[:0x2000 + 0x1000], "no multiboot header"
    entry, phoff = struct.unpack_from("<II", data, 24)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for i in range(phnum):
        p_type, off, _, paddr, filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1:
            uc.mem_write(paddr, data[off:off + filesz])
    return entry


class Machine:
    def __init__(self, path):
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(0, RAM)
        self.serial = bytearray()
        self.keys = []
        self.dlab = False
        self.masks = {0x21: 0xFF, 0xA1: 0xFF}
        uc.mem_write(MBI, struct.pack("<III", 1, 640, RAM // 1024 - 1024))
        uc.reg_write(UC_X86_REG_EAX, 0x2BADB002)
        uc.reg_write(UC_X86_REG_EBX, MBI)
        uc.hook_add(UC_HOOK_INSN, self.port_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, self.port_out, None, 1, 0, UC_X86_INS_OUT)
        self.pc = load_elf(uc, path)

    def port_in(self, uc, port, size, _):
        if port == 0x60:
            return self.keys.pop(0) if self.keys else 0
        if port == 0x3FD:
            return 0x20                 # UART transmitter empty
        return self.masks.get(port, 0)

    def port_out(self, uc, port, size, value, _):
        if port == 0x3FB:
            self.dlab = bool(value & 0x80)
        elif port == 0x3F8 and not self.dlab:
            self.serial.append(value & 0xFF)
        elif port in self.masks:
            self.masks[port] = value & 0xFF

    def run(self):
        """Run until the CPU halts; return True if interrupts are enabled."""
        try:
            self.uc.emu_start(self.pc, 0xFFFFFFFF, count=20_000_000)
        except UcError as e:
            # Unicorn reports faults to the host instead of using the guest
            # IDT, so raise #UD (vector 6) on the kernel's behalf.
            if e.errno != UC_ERR_INSN_INVALID:
                raise
            self.pc = self.uc.reg_read(UC_X86_REG_EIP)
            return self.interrupt(6)
        self.pc = self.uc.reg_read(UC_X86_REG_EIP)
        code = self.uc.mem_read(self.pc - 1, 1)[0]
        assert code == 0xF4, "instruction budget exhausted at %#x (hang?)" % self.pc
        return bool(self.uc.reg_read(UC_X86_REG_EFLAGS) & 0x200)

    def irq(self, n):
        """Deliver hardware IRQ n (PIC remapped to vectors 32+)."""
        assert not self.masks[0x21] & (1 << n), "IRQ%d is masked" % n
        return self.interrupt(32 + n)

    def interrupt(self, vector):
        """Enter the kernel through its IDT the way a real CPU would."""
        uc = self.uc
        _, base, _, _ = uc.reg_read(UC_X86_REG_IDTR)
        lo, sel, _, typ, hi = struct.unpack("<HHBBH", uc.mem_read(base + vector * 8, 8))
        assert typ == 0x8E and sel == 0x08, "bad IDT gate %d" % vector
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        esp = uc.reg_read(UC_X86_REG_ESP) - 12
        uc.mem_write(esp, struct.pack("<III", self.pc, uc.reg_read(UC_X86_REG_CS), flags))
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_EFLAGS, flags & ~0x200)
        self.pc = lo | hi << 16
        return self.run()

    def type(self, text):
        for ch in text:
            sc = SCANCODES[ch]
            for code in (sc, sc | 0x80):        # press + release
                self.keys.append(code)
                assert self.irq(1), "kernel stopped while typing %r" % text

    def screen(self):
        raw = self.uc.mem_read(0xB8000, 80 * 25 * 2)
        rows = [bytes(raw[r * 160:(r + 1) * 160:2]).decode("cp437").rstrip() for r in range(25)]
        return "\n".join(rows).rstrip()


def main():
    m = Machine(sys.argv[1] if len(sys.argv) > 1 else "build/vortex.bin")
    assert m.run(), "kernel halted with interrupts off:\n" + m.screen()
    assert m.screen().endswith("vortex>"), m.screen()

    for _ in range(250):                        # 2.5 s worth of timer ticks
        m.irq(0)
    m.type("help\n")
    m.type("echo hello vortex\n")
    m.type("uptime\n")
    m.type("mem\n")
    m.type("nosuchcmd\n")
    out = m.serial.decode("ascii", "replace")
    print(out)
    for needle in ("[ OK ] PS/2 keyboard", "Memory: 32 MiB", "hello vortex",
                   "up 00:00:02 (250 ticks)", "Heap: 16384 KiB total",
                   "nosuchcmd: command not found"):
        assert needle in out, "missing %r in output" % needle
    assert "hello vortex" in m.screen()

    m.type("crash")
    m.keys += [0x1C]                            # Enter: this one never returns
    assert not m.irq(1), "exception handler should halt with interrupts off"
    assert "CPU EXCEPTION 6: Invalid opcode" in m.screen(), m.screen()
    print("\n--- final screen ---\n" + m.screen())
    print("\nPASS")


if __name__ == "__main__":
    main()
