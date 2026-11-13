#!/usr/bin/env python3
"""Create and inspect FAT12/16/32 disk images without mkfs.fat / mtools.

    mkfat.py create IMAGE DIR [SIZE_MB] [--fat 12|16|32] [--mbr]
    mkfat.py ls IMAGE                     list all files (path and size)
    mkfat.py cat IMAGE PATH               print one file
    mkfat.py check IMAGE                  verify the volume structure

The reader is independent from the kernel's FAT code, so the test uses it to
check what VortexOS wrote to the disk.
"""
import os
import struct
import sys
import time

SECTOR = 512
ROOT_ENTRIES = 512
LFN_POS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
SHORT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&@^'(){}`")
MBR_START = 2048        # first sector of the partition with --mbr


def checksum(name11):
    total = 0
    for b in name11:
        total = (((total & 1) << 7) + (total >> 1) + b) & 0xFF
    return total


def name_83(name):
    """11-byte short name and case flags, or None if a long name is needed."""
    base, dot, ext = name.rpartition(".") if "." in name else (name, "", "")
    if not base or len(base) > 8 or len(ext) > 3 or (dot and not ext):
        return None
    flags = 0
    for part, bit in ((base, 0x08), (ext, 0x10)):
        if not all(c in SHORT_CHARS for c in part.upper()):
            return None
        if part != part.upper():
            if part != part.lower():
                return None
            flags |= bit
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode(), flags


def alias_83(name, index):
    base, _, ext = name.rpartition(".") if "." in name[1:] else (name, "", "")
    num = "~%d" % index
    base = "".join(c for c in base.upper() if c in SHORT_CHARS)[:8 - len(num)] + num
    ext = "".join(c for c in ext.upper() if c in SHORT_CHARS)[:3]
    return (base.ljust(8) + ext.ljust(3)).encode()


def pack_fat(entries, bits):
    if bits == 16:
        return struct.pack("<%dH" % len(entries), *entries)
    if bits == 32:
        return struct.pack("<%dI" % len(entries), *entries)
    out = bytearray()
    for i in range(0, len(entries), 2):
        a = entries[i]
        b = entries[i + 1] if i + 1 < len(entries) else 0
        out += bytes((a & 0xFF, (a >> 8) | ((b & 0xF) << 4), b >> 4))
    return bytes(out)


def unpack_fat(raw, bits, count):
    if bits == 16:
        return list(struct.unpack_from("<%dH" % count, raw))
    if bits == 32:
        return [x & 0x0FFFFFFF for x in struct.unpack_from("<%dI" % count, raw)]
    out = []
    for c in range(count):
        x = raw[c + c // 2] | raw[c + c // 2 + 1] << 8
        out.append(x >> 4 if c & 1 else x & 0xFFF)
    return out


class Builder:
    """Builds a volume of `sectors` sectors; `bits` is 12, 16 or 32."""

    def __init__(self, size_mb=16, bits=16, sectors=None, spc=None):
        self.bits = bits
        self.total = sectors or size_mb * 1024 * 1024 // SECTOR
        self.spc = spc or {12: 1, 16: 4, 32: 1}[bits]
        self.reserved = 32 if bits == 32 else 1
        self.fats = 2
        self.root_sectors = 0 if bits == 32 else ROOT_ENTRIES * 32 // SECTOR
        self.fat_sectors = 1
        while True:                     # the FAT must cover every cluster
            self.data_start = self.reserved + self.fats * self.fat_sectors + self.root_sectors
            self.clusters = (self.total - self.data_start) // self.spc
            if self.fat_sectors * SECTOR * 8 // bits >= self.clusters + 2:
                break
            self.fat_sectors += 1
        low, high = {12: (1, 4085), 16: (4085, 65525), 32: (65525, 1 << 28)}[bits]
        assert low <= self.clusters < high, \
            "%d clusters do not make a FAT%d volume: change the size" % (self.clusters, bits)
        self.eoc = (1 << min(bits, 28)) - 1
        self.fat = [self.eoc - 7, self.eoc]
        self.data = bytearray()
        now = time.localtime()
        self.date = (now.tm_year - 1980) << 9 | now.tm_mon << 5 | now.tm_mday
        self.time = now.tm_hour << 11 | now.tm_min << 5 | now.tm_sec // 2

    def alloc(self, blob):
        """Store `blob` in a new cluster chain; returns its first cluster."""
        size = self.spc * SECTOR
        count = (len(blob) + size - 1) // size
        first = len(self.fat)
        assert first + count <= self.clusters + 2, "disk image is too small"
        self.fat += [first + i + 1 for i in range(count - 1)] + [self.eoc]
        self.data += blob.ljust(count * size, b"\0")
        return first

    def entry(self, name11, flags, attr, cluster, size):
        return struct.pack("<11sBBBHHHHHHHI", name11, attr, flags, 0, self.time, self.date,
                           self.date, cluster >> 16, self.time, self.date, cluster & 0xFFFF, size)

    def directory(self, path, self_cluster, parent_cluster):
        """Directory entries for `path`; children are written first."""
        out = b""
        if self_cluster is None:
            out += self.entry(b"VORTEXOS   ", 0, 0x08, 0, 0)
        for index, name in enumerate(sorted(os.listdir(path)), 1):
            full = os.path.join(path, name)
            is_dir = os.path.isdir(full)
            if is_dir:
                # reserve the directory's own clusters before filling it in
                cluster = self.alloc(b"\0" * self.dir_size(full))
                blob = self.directory(full, cluster, self_cluster or 0)
                off = (cluster - 2) * self.spc * SECTOR
                self.data[off:off + len(blob)] = blob
                size = 0
            else:
                blob = open(full, "rb").read()
                cluster = self.alloc(blob) if blob else 0
                size = len(blob)
            short = name_83(name)
            if short is None:
                name11, flags = alias_83(name, index), 0
                parts = (len(name) + 12) // 13
                for seq in range(parts, 0, -1):
                    chars = [ord(c) for c in name[(seq - 1) * 13:seq * 13]]
                    if len(chars) < 13:
                        chars = (chars + [0] + [0xFFFF] * 13)[:13]
                    e = bytearray(32)
                    e[0] = seq | (0x40 if seq == parts else 0)
                    e[11], e[13] = 0x0F, checksum(name11)
                    for pos, ch in zip(LFN_POS, chars):
                        struct.pack_into("<H", e, pos, ch)
                    out += bytes(e)
            else:
                name11, flags = short
            out += self.entry(name11, flags, 0x10 if is_dir else 0x20, cluster, size)
        if self_cluster is not None:
            out = (self.entry(b".          ", 0, 0x10, self_cluster, 0) +
                   self.entry(b"..         ", 0, 0x10, parent_cluster, 0) + out)
        return out

    def dir_size(self, path):
        count = 2
        for name in os.listdir(path):
            count += 1 if name_83(name) else 1 + (len(name) + 12) // 13
        return count * 32

    def build(self, src, mbr=False):
        root_cluster = 0
        if self.bits == 32:             # the root directory is an ordinary chain
            root_cluster = self.alloc(b"\0" * (self.dir_size(src) + 32))
            root = self.directory(src, None, 0)
            self.data[0:len(root)] = root
            root = b""
        else:
            root = self.directory(src, None, 0)
            assert len(root) <= ROOT_ENTRIES * 32, "too many files in the root directory"

        hidden = MBR_START if mbr else 0
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x58\x90" if self.bits == 32 else b"\xEB\x3C\x90"
        boot[3:11] = b"VORTEXOS"
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, self.spc, self.reserved, self.fats,
                         0 if self.bits == 32 else ROOT_ENTRIES,
                         self.total if self.total < 65536 else 0, 0xF8,
                         0 if self.bits == 32 else self.fat_sectors,
                         63, 255, hidden, self.total if self.total >= 65536 else 0)
        serial = int(time.time()) & 0xFFFFFFFF
        if self.bits == 32:
            struct.pack_into("<IHHIHH12x", boot, 36, self.fat_sectors, 0, 0, root_cluster, 1, 6)
            struct.pack_into("<BBBI11s8s", boot, 64, 0x80, 0, 0x29, serial, b"VORTEXOS   ", b"FAT32   ")
        else:
            struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, serial, b"VORTEXOS   ",
                             b"FAT%d   " % self.bits)
        boot[510:512] = b"\x55\xAA"

        reserved = bytearray(self.reserved * SECTOR)
        reserved[0:SECTOR] = boot
        if self.bits == 32:
            info = bytearray(SECTOR)
            struct.pack_into("<I", info, 0, 0x41615252)
            struct.pack_into("<III", info, 484, 0x61417272,
                             self.clusters + 2 - len(self.fat), len(self.fat))
            struct.pack_into("<I", info, 508, 0xAA550000)
            reserved[SECTOR:2 * SECTOR] = info
            reserved[6 * SECTOR:7 * SECTOR] = boot          # backup boot sector
            reserved[7 * SECTOR:8 * SECTOR] = info
        fat = pack_fat(self.fat, self.bits).ljust(self.fat_sectors * SECTOR, b"\0")
        volume = (bytes(reserved) + fat * self.fats + root.ljust(self.root_sectors * SECTOR, b"\0")
                  + bytes(self.data)).ljust(self.total * SECTOR, b"\0")
        if not mbr:
            return volume
        table = bytearray(SECTOR)
        kind = {12: 0x01, 16: 0x06, 32: 0x0C}[self.bits]
        struct.pack_into("<B3sB3sII", table, 446, 0x00, b"\xFE\xFF\xFF", kind, b"\xFE\xFF\xFF",
                         MBR_START, self.total)
        table[510:512] = b"\x55\xAA"
        return bytes(table).ljust(MBR_START * SECTOR, b"\0") + volume


class Volume:
    """Reads and verifies a FAT volume (optionally inside an MBR partition)."""

    def __init__(self, image):
        assert image[510:512] == b"\x55\xAA", "no boot signature"
        self.base = 0
        if image[0] not in (0xEB, 0xE9) or struct.unpack_from("<H", image, 11)[0] != SECTOR:
            for i in range(4):          # not a boot sector: look for a FAT partition
                kind, start = image[446 + i * 16 + 4], struct.unpack_from("<I", image, 446 + i * 16 + 8)[0]
                if kind in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E):
                    self.base = start
                    break
            else:
                raise AssertionError("no FAT volume or partition")
        self.image = image
        b = self.sectors(0, 1)
        assert b[510:512] == b"\x55\xAA", "no boot signature in the volume"
        bps, self.spc, reserved, fats, root_entries, total16, _, fat16 = \
            struct.unpack_from("<HBHBHHBH", b, 11)
        assert bps == SECTOR
        total = total16 or struct.unpack_from("<I", b, 32)[0]
        fat_sectors = fat16 or struct.unpack_from("<I", b, 36)[0]
        self.root_start = reserved + fats * fat_sectors
        self.data_start = self.root_start + (root_entries * 32 + SECTOR - 1) // SECTOR
        self.clusters = (total - self.data_start) // self.spc
        self.bits = 12 if self.clusters < 4085 else 16 if self.clusters < 65525 else 32
        self.eoc = ((1 << min(self.bits, 28)) - 1) - 7
        copies = [self.sectors(reserved + i * fat_sectors, fat_sectors) for i in range(fats)]
        assert all(c == copies[0] for c in copies), "FAT copies differ"
        self.fat = unpack_fat(copies[0], self.bits, self.clusters + 2)
        self.root_cluster = struct.unpack_from("<I", b, 44)[0] if self.bits == 32 else 0
        self.fsinfo = struct.unpack_from("<H", b, 48)[0] if self.bits == 32 else 0
        self.used = set()
        self.tree = {}
        if self.bits == 32:
            self.walk(self.chain(self.root_cluster), "")
        else:
            self.walk(self.sectors(self.root_start, self.data_start - self.root_start), "")

    def sectors(self, lba, count):
        return self.image[(self.base + lba) * SECTOR:(self.base + lba + count) * SECTOR]

    def chain(self, cluster):
        out = b""
        while 2 <= cluster < self.eoc:
            assert cluster < self.clusters + 2, "cluster %d is outside the volume" % cluster
            assert cluster not in self.used, "cluster %d is used twice" % cluster
            self.used.add(cluster)
            out += self.sectors(self.data_start + (cluster - 2) * self.spc, self.spc)
            cluster = self.fat[cluster]
            assert cluster >= 2, "chain runs into a free cluster"
        return out

    def walk(self, entries, prefix):
        lfn, shorts = {}, set()
        for off in range(0, len(entries), 32):
            e = entries[off:off + 32]
            if e[0] == 0:
                assert not any(entries[off:]), "%s/: data after the end-of-directory mark" % prefix
                break
            if e[0] == 0xE5:
                lfn = {}
                continue
            if e[11] == 0x0F:
                if e[0] & 0x40:
                    lfn = {"sum": e[13]}
                lfn[e[0] & 0x1F] = [struct.unpack_from("<H", e, p)[0] for p in LFN_POS]
                continue
            long_name = None
            if lfn:
                assert lfn.get("sum") == checksum(e[:11]), "%s/: orphaned long-name entries" % prefix
                chars = [c for seq in sorted(k for k in lfn if k != "sum") for c in lfn[seq]]
                if 0 in chars:
                    chars = chars[:chars.index(0)]
                long_name = "".join(chr(c) for c in chars)
            lfn = {}
            if e[11] & 0x08:
                continue
            base, ext = e[:8].decode("latin-1").rstrip(), e[8:11].decode("latin-1").rstrip()
            if base not in (".", ".."):
                assert bytes(e[:11]) not in shorts, "%s/: duplicate short name %r" % (prefix, e[:11])
                shorts.add(bytes(e[:11]))
            if e[12] & 0x08:
                base = base.lower()
            if e[12] & 0x10:
                ext = ext.lower()
            name = long_name or (base + "." + ext if ext else base)
            hi, _, _, lo, size = struct.unpack_from("<HHHHI", e, 20)
            cluster = lo | (hi << 16 if self.bits == 32 else 0)
            if name in (".", ".."):
                continue
            path = prefix + "/" + name
            assert path.lower() not in (p.lower() for p in self.tree), "duplicate name " + path
            if e[11] & 0x10:
                self.tree[path] = None
                body = self.chain(cluster)
                dot, dotdot = struct.unpack_from("<H", body, 26)[0], struct.unpack_from("<H", body, 32 + 26)[0]
                assert body[:11] == b".          " and dot == cluster & 0xFFFF, path + ": bad '.' entry"
                assert body[32:43] == b"..         ", path + ": bad '..' entry"
                self.walk(body, path)
            else:
                data = self.chain(cluster)
                assert len(data) >= size, "%s: chain shorter than the file" % path
                assert len(data) - size < self.spc * SECTOR, "%s: chain longer than the file" % path
                self.tree[path] = data[:size]

    def check(self):
        """Everything allocated must belong to a file, and FSInfo must agree."""
        allocated = {c for c in range(2, self.clusters + 2) if self.fat[c]}
        lost = allocated - self.used
        assert not lost, "%d lost cluster(s), e.g. %d" % (len(lost), min(lost))
        free = self.clusters - len(allocated)
        if self.fsinfo:
            info = self.sectors(self.fsinfo, 1)
            assert struct.unpack_from("<I", info, 0)[0] == 0x41615252, "bad FSInfo signature"
            count = struct.unpack_from("<I", info, 488)[0]
            assert count in (free, 0xFFFFFFFF), "FSInfo says %d free clusters, FAT says %d" % (count, free)
        return free * self.spc * SECTOR


def read_tree(image, check=True):
    """Parse an image: {path: bytes for files, None for directories}."""
    volume = Volume(image)
    if check:
        volume.check()
    return volume.tree


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) >= 3 and args[0] == "create":
        bits = int(argv[argv.index("--fat") + 1]) if "--fat" in argv else 16
        if "--fat" in argv:
            args.remove(str(bits))
        size = int(args[3]) if len(args) > 3 else {12: 2, 16: 16, 32: 40}[bits]
        image = Builder(size, bits).build(args[2], mbr="--mbr" in argv)
        open(args[1], "wb").write(image)
        print("%s: FAT%d, %d MiB, %d file(s)" % (args[1], bits, len(image) >> 20,
              sum(v is not None for v in read_tree(image).values())))
    elif len(args) == 2 and args[0] == "ls":
        for path, data in sorted(read_tree(open(args[1], "rb").read()).items()):
            print("%8s  %s" % ("<DIR>" if data is None else len(data), path))
    elif len(args) == 3 and args[0] == "cat":
        sys.stdout.buffer.write(read_tree(open(args[1], "rb").read())[args[2]])
    elif len(args) == 2 and args[0] == "check":
        volume = Volume(open(args[1], "rb").read())
        free = volume.check()
        print("%s: FAT%d, %d cluster(s), %d KiB free, %d file(s), no errors"
              % (args[1], volume.bits, volume.clusters, free // 1024,
                 sum(v is not None for v in volume.tree.values())))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
