#!/usr/bin/env python3
"""Build helper: the file operations the Makefile needs, without Unix tools.

The same commands work in cmd.exe, PowerShell and a Unix shell, so the build
needs only make, the compiler and Python.

    build.py mkdir DIR...                  create directories (like mkdir -p)
    build.py rmtree DIR...                 delete directories (like rm -rf)
    build.py copy SRC DST                  copy one file
    build.py initrd OUT ROOTFS [DIR=FILE,FILE...]...
                                           ustar archive of ROOTFS plus extra
                                           files placed into archive directory DIR
    build.py cc COMPILER                   print the compiler's target (or "missing")
    build.py qemu [QEMU] ARGS...           find QEMU and run it
"""
import io
import os
import shutil
import subprocess
import sys
import tarfile


def initrd(out, rootfs, extras):
    """Write a deterministic ustar archive; names start with "./" like `tar -C dir .`."""
    files = {}                                  # archive path -> file on disk
    dirs = set()
    for base, subdirs, names in os.walk(rootfs):
        rel = os.path.relpath(base, rootfs).replace(os.sep, "/")
        prefix = "" if rel == "." else rel + "/"
        if prefix:
            dirs.add(prefix.rstrip("/"))
        for name in names:
            files[prefix + name] = os.path.join(base, name)
    for extra in extras:
        target, _, sources = extra.partition("=")
        dirs.add(target)
        for src in filter(None, sources.split(",")):
            files[target + "/" + os.path.basename(src)] = src

    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        def add(name, kind, data=b""):
            info = tarfile.TarInfo("./" + name)
            info.type = kind
            info.mode = 0o755
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))

        for name in sorted(dirs):
            add(name, tarfile.DIRTYPE)
        for name in sorted(files):
            with open(files[name], "rb") as f:
                add(name, tarfile.REGTYPE, f.read())
    print("%s: %d file(s)" % (out, len(files)))


def compiler_target(cc):
    try:
        out = subprocess.run([cc, "-dumpmachine"], capture_output=True, text=True)
        return out.stdout.strip() or "missing"
    except OSError:
        return "missing"


def find_qemu(name):
    found = shutil.which(name)
    if found:
        return found
    for var in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
        base = os.environ.get(var)
        candidate = os.path.join(base, "qemu", os.path.basename(name) + ".exe") if base else ""
        if candidate and os.path.exists(candidate):
            return candidate
    return None


def qemu(args):
    name = "qemu-system-x86_64"
    if args and not args[0].startswith("-"):
        name, args = args[0], args[1:]
    exe = find_qemu(name)
    if not exe:
        sys.exit("%s not found. Install QEMU (https://www.qemu.org/download/) "
                 "or pass its path: make run QEMU=C:/path/to/qemu-system-x86_64.exe" % name)
    return subprocess.call([exe] + args)


def main(argv):
    cmd, args = (argv[1], argv[2:]) if len(argv) > 1 else ("", [])
    if cmd == "mkdir":
        for path in args:
            os.makedirs(path, exist_ok=True)
        print("ok")
    elif cmd == "rmtree":
        for path in args:
            shutil.rmtree(path, ignore_errors=True)
    elif cmd == "copy" and len(args) == 2:
        os.makedirs(os.path.dirname(args[1]) or ".", exist_ok=True)
        shutil.copyfile(args[0], args[1])
    elif cmd == "initrd" and len(args) >= 2:
        initrd(args[0], args[1], args[2:])
    elif cmd == "cc" and len(args) == 1:
        print(compiler_target(args[0]))
    elif cmd == "qemu":
        return qemu(args)
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
