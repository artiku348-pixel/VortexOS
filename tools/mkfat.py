#!/usr/bin/env python3
"""Create and inspect FAT16 disk images without mkfs.fat / mtools.

    mkfat.py create IMAGE DIR [SIZE_MB]   build an image from a directory
    mkfat.py ls IMAGE                     list all files (path and size)
    mkfat.py cat IMAGE PATH               print one file

The reader is independent from the kernel's FAT code, so the test uses it to
check what VortexOS wrote to the disk.
"""
import os
import struct
import sys
import time

SECTOR = 512
SPC = 4                 # sectors per cluster (2 KiB)
RESERVED = 1
FATS = 2
ROOT_ENTRIES = 512
LFN_POS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
SHORT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&@^")


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
    base, _, ext = name.rpartition(".") if "." in name else (name, "", "")
    num = "~%d" % index
    base = "".join(c for c in base.upper() if c in SHORT_CHARS)[:8 - len(num)] + num
    ext = "".join(c for c in ext.upper() if c in SHORT_CHARS)[:3]
    return (base.ljust(8) + ext.ljust(3)).encode()


class Builder:
    def __init__(self, size_mb):
        self.total = size_mb * 1024 * 1024 // SECTOR
        root_sectors = ROOT_ENTRIES * 32 // SECTOR
        self.fat_sectors = 1
        while True:                     # the FAT must cover every cluster
            self.data_start = RESERVED + FATS * self.fat_sectors + root_sectors
            self.clusters = (self.total - self.data_start) // SPC
            if self.fat_sectors * SECTOR // 2 >= self.clusters + 2:
                break
            self.fat_sectors += 1
        assert 4085 <= self.clusters < 65525, "size must give a FAT16 volume (9..127 MiB)"
        self.fat = [0xFFF8, 0xFFFF]
        self.data = bytearray()
        now = time.localtime()
        self.date = (now.tm_year - 1980) << 9 | now.tm_mon << 5 | now.tm_mday
        self.time = now.tm_hour << 11 | now.tm_min << 5 | now.tm_sec // 2

    def alloc(self, blob):
        """Store `blob` in a new cluster chain; returns its first cluster."""
        size = SPC * SECTOR
        count = (len(blob) + size - 1) // size
        first = len(self.fat)
        assert first + count <= self.clusters + 2, "disk image is too small"
        self.fat += [first + i + 1 for i in range(count - 1)] + [0xFFFF]
        self.data += blob.ljust(count * size, b"\0")
        return first

    def entry(self, name11, flags, attr, cluster, size):
        return struct.pack("<11sBBBHHHHHHHI", name11, attr, flags, 0, self.time, self.date,
                           self.date, 0, self.time, self.date, cluster, size)

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
                off = (cluster - 2) * SPC * SECTOR
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

    def build(self, src):
        root = self.directory(src, None, 0)
        assert len(root) <= ROOT_ENTRIES * 32, "too many files in the root directory"
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x3C\x90"
        boot[3:11] = b"VORTEXOS"
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, SPC, RESERVED, FATS, ROOT_ENTRIES,
                         self.total if self.total < 65536 else 0, 0xF8, self.fat_sectors,
                         63, 255, 0, self.total if self.total >= 65536 else 0)
        struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, int(time.time()) & 0xFFFFFFFF,
                         b"VORTEXOS   ", b"FAT16   ")
        boot[510:512] = b"\x55\xAA"
        fat = struct.pack("<%dH" % len(self.fat), *self.fat).ljust(self.fat_sectors * SECTOR, b"\0")
        image = bytes(boot) + fat * FATS + root.ljust(ROOT_ENTRIES * 32, b"\0") + bytes(self.data)
        return image.ljust(self.total * SECTOR, b"\0")


def read_tree(image):
    """Parse a FAT16 image: {path: bytes for files, None for directories}."""
    assert image[510:512] == b"\x55\xAA", "no boot signature"
    bps, spc, reserved, fats, root_entries, total16, _, fat_sectors = \
        struct.unpack_from("<HBHBHHBH", image, 11)
    assert bps == SECTOR
    root_start = reserved + fats * fat_sectors
    data_start = root_start + (root_entries * 32 + SECTOR - 1) // SECTOR
    fat_at = reserved * SECTOR
    copies = [image[fat_at + i * fat_sectors * SECTOR:fat_at + (i + 1) * fat_sectors * SECTOR]
              for i in range(fats)]
    assert all(c == copies[0] for c in copies), "FAT copies differ"
    used = set()

    def chain(cluster):
        out = b""
        while 2 <= cluster < 0xFFF8:
            assert cluster not in used, "cluster %d is used twice" % cluster
            used.add(cluster)
            off = (data_start + (cluster - 2) * spc) * SECTOR
            out += image[off:off + spc * SECTOR]
            cluster = struct.unpack_from("<H", copies[0], cluster * 2)[0]
            assert cluster >= 2, "chain runs into a free cluster"
        return out

    tree = {}

    def walk(entries, prefix):
        lfn = {}
        for off in range(0, len(entries), 32):
            e = entries[off:off + 32]
            if e[0] == 0:
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
            if lfn.get("sum") == checksum(e[:11]):
                chars = [c for seq in sorted(k for k in lfn if k != "sum") for c in lfn[seq]]
                if 0 in chars:
                    chars = chars[:chars.index(0)]
                long_name = "".join(chr(c) for c in chars)
            lfn = {}
            if e[11] & 0x08:
                continue
            base, ext = e[:8].decode("latin-1").rstrip(), e[8:11].decode("latin-1").rstrip()
            if e[12] & 0x08:
                base = base.lower()
            if e[12] & 0x10:
                ext = ext.lower()
            name = long_name or (base + "." + ext if ext else base)
            if name in (".", ".."):
                continue
            cluster, size = struct.unpack_from("<HI", e, 26)
            path = prefix + "/" + name
            if e[11] & 0x10:
                tree[path] = None
                walk(chain(cluster), path)
            else:
                tree[path] = chain(cluster)[:size]
                assert len(tree[path]) == size, "%s: chain shorter than the file" % path

    walk(image[root_start * SECTOR:data_start * SECTOR], "")
    return tree


def main(argv):
    if len(argv) >= 4 and argv[1] == "create":
        image = Builder(int(argv[4]) if len(argv) > 4 else 16).build(argv[3])
        open(argv[2], "wb").write(image)
        print("%s: FAT16, %d MiB, %d file(s)" % (argv[2], len(image) >> 20,
              sum(v is not None for v in read_tree(image).values())))
    elif len(argv) == 3 and argv[1] == "ls":
        for path, data in sorted(read_tree(open(argv[2], "rb").read()).items()):
            print("%8s  %s" % ("<DIR>" if data is None else len(data), path))
    elif len(argv) == 4 and argv[1] == "cat":
        sys.stdout.buffer.write(read_tree(open(argv[2], "rb").read())[argv[3]])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
