#!/usr/bin/env python3
"""Smoke test: boot VortexOS inside the Unicorn CPU emulator (no QEMU needed).

Unicorn emulates only the CPU, so this script plays the rest of the PC:
the Multiboot bootloader (kernel + initrd module), the interrupt controller,
the PS/2 keyboard and mouse, the CMOS clock, the serial port and a Bochs VBE
graphics card with a linear framebuffer. Whenever the kernel idles in `hlt`
we deliver the next timer / keyboard / mouse interrupt through the kernel's
own IDT. Screenshots of the framebuffer are written to build/*.png.
"""
import os
import struct
import sys
import zlib

from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INSN, UC_HOOK_INTR,
                     UC_ERR_INSN_INVALID)
from unicorn.x86_const import *

RAM = 128 * 1024 * 1024
MBI = 0x9000
LFB = 0xFD000000
W, H = 1024, 768

LOWER = "\x00\x001234567890-=\x00\x00qwertyuiop[]\n\x00asdfghjkl;'`\x00\\zxcvbnm,./"
UPPER = "\x00\x00!@#$%^&*()_+\x00\x00QWERTYUIOP{}\n\x00ASDFGHJKL:\"~\x00|ZXCVBNM<>?"
PLAIN = {c: i for i, c in enumerate(LOWER) if c != "\x00"}
SHIFT = {c: i for i, c in enumerate(UPPER) if c != "\x00" and c != "\n"}
PLAIN[" "] = 0x39
EXTENDED = {"UP": 0x48, "DOWN": 0x50, "LEFT": 0x4B, "RIGHT": 0x4D}


def load_elf(uc, data):
    assert data[:4] == b"\x7fELF" and data[4] == 1, "expected a 32-bit ELF"
    assert struct.pack("<I", 0x1BADB002) in data[:0x3000], "no multiboot header"
    entry, phoff = struct.unpack_from("<II", data, 24)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    end = 0
    for i in range(phnum):
        p_type, off, _, paddr, filesz, memsz, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1:
            uc.mem_write(paddr, data[off:off + filesz])
            end = max(end, paddr + memsz)
    return entry, end


class Machine:
    def __init__(self, kernel, initrd=None):
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
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
        self.redirect = None
        self.mouse = [W // 2, H // 2]   # where the kernel believes the pointer is
        self.buttons = 0

        self.pc, kernel_end = load_elf(uc, open(kernel, "rb").read())
        flags, mods = 1, b""
        if initrd:
            blob = open(initrd, "rb").read()
            start = (kernel_end + 0xFFF) & ~0xFFF
            uc.mem_write(start, blob)
            uc.mem_write(MBI + 0x100, struct.pack("<IIII", start, start + len(blob), 0, 0))
            flags |= 8
            mods = struct.pack("<II", 1, MBI + 0x100)
        uc.mem_write(MBI, struct.pack("<IIIII", flags, 640, RAM // 1024 - 1024, 0, 0) + mods)
        uc.reg_write(UC_X86_REG_EAX, 0x2BADB002)
        uc.reg_write(UC_X86_REG_EBX, MBI)
        uc.hook_add(UC_HOOK_INSN, self.port_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, self.port_out, None, 1, 0, UC_X86_INS_OUT)
        uc.hook_add(UC_HOOK_INTR, self.soft_int)

    # ---- devices ---------------------------------------------------------
    def port_in(self, uc, port, size, _):
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
    def enter_gate(self, vector, return_pc):
        """Push an interrupt frame and return the handler address."""
        uc = self.uc
        _, base, _, _ = uc.reg_read(UC_X86_REG_IDTR)
        lo, sel, _, typ, hi = struct.unpack("<HHBBH", uc.mem_read(base + vector * 8, 8))
        assert typ in (0x8E, 0x8F) and sel == 0x08, "bad IDT gate %d" % vector
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        esp = uc.reg_read(UC_X86_REG_ESP) - 12
        uc.mem_write(esp, struct.pack("<III", return_pc, uc.reg_read(UC_X86_REG_CS), flags))
        uc.reg_write(UC_X86_REG_ESP, esp)
        if typ == 0x8E:                 # interrupt gates clear IF, trap gates do not
            uc.reg_write(UC_X86_REG_EFLAGS, flags & ~0x200)
        return lo | hi << 16

    def soft_int(self, uc, intno, _):
        # Unicorn reports `int N` to the host; route it through the guest IDT.
        self.redirect = self.enter_gate(intno, uc.reg_read(UC_X86_REG_EIP))
        uc.emu_stop()

    def run(self):
        """Run until the CPU halts; return True if interrupts are enabled."""
        while True:
            self.redirect = None
            try:
                self.uc.emu_start(self.pc, 0xFFFFFFFF, timeout=120 * 1000000)
            except UcError as e:
                # Faults are also reported to the host: raise #UD ourselves.
                if e.errno != UC_ERR_INSN_INVALID:
                    raise
                self.pc = self.enter_gate(6, self.uc.reg_read(UC_X86_REG_EIP))
                continue
            if self.redirect is not None:
                self.pc = self.redirect
                continue
            self.pc = self.uc.reg_read(UC_X86_REG_EIP)
            assert self.uc.mem_read(self.pc - 1, 1)[0] == 0xF4, "timeout at %#x (hang?)" % self.pc
            return bool(self.uc.reg_read(UC_X86_REG_EFLAGS) & 0x200)

    def irq(self, n):
        mask = self.masks[0x21] if n < 8 else self.masks[0xA1]
        assert not mask & (1 << (n & 7)), "IRQ%d is masked" % n
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
           "up 00:00:02", "2026-11-13 12:34:56", "   1  idle",
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
    print("\nPASS")


if __name__ == "__main__":
    main()
