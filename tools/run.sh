#!/bin/sh
# Build VortexOS and boot it in QEMU.
#   tools/run.sh          kernel + initrd directly (fastest)
#   tools/run.sh iso      bootable ISO through GRUB
#   tools/run.sh debug    wait for gdb on :1234 (gdb build/vortex.bin -ex 'target remote :1234')
set -e
cd "$(dirname "$0")/.."

command -v "${QEMU:-qemu-system-i386}" >/dev/null 2>&1 || {
    echo "qemu-system-i386 not found. Install QEMU (e.g. 'apt install qemu-system-x86')." >&2
    exit 1
}
case "${1:-}" in
    "")    exec make run ;;
    iso)   exec make run-iso ;;
    debug) exec make run QEMUFLAGS="-m 128 -vga std -serial stdio -s -S" ;;
    *)     echo "usage: $0 [iso|debug]" >&2; exit 1 ;;
esac
